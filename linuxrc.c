/*
 *
 * linuxrc.c     Load modules and rootimage to ramdisk
 *
 * Copyright (c) 1996-2001  Hubert Mantel, SuSE GmbH  (mantel@suse.de)
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <inttypes.h>
#include <ctype.h>

#include "global.h"
#include "text.h"
#include "info.h"
#include "util.h"
#include "display.h"
#include "keyboard.h"
#include "dialog.h"
#include "window.h"
#include "module.h"
#include "net.h"
#if WITH_PCMCIA
#include "pcmcia.h"
#endif
#include "install.h"
#include "settings.h"
#include "file.h"
#include "linuxrc.h"
#include "auto2.h"
#include "lsh.h"
#include "multiple_info.h"

static void lxrc_main_menu     (void);
static void lxrc_init          (void);
static int  lxrc_main_cb       (int what_iv);
static void lxrc_memcheck      (void);
static void lxrc_check_console (void);
static void lxrc_set_bdflush   (int percent_iv);
static int is_rpc_prog         (pid_t pid);
static void save_environment   (void);
static void lxrc_reboot        (void);
static void lxrc_halt          (void);

static pid_t  lxrc_mempid_rm;
static int    lxrc_sig11_im = FALSE;
static char **lxrc_argv;
const char *lxrc_new_root;
static char **saved_environment;
extern char **environ;
static void   lxrc_movetotmpfs(void);

#if SWISS_ARMY_KNIFE
int cardmgr_main(int argc, char **argv);
int insmod_main(int argc, char **argv);
int loadkeys_main(int argc, char **argv);
int portmap_main(int argc, char **argv);
int probe_main(int argc, char **argv);
int rmmod_main(int argc, char **argv);
int setfont_main(int argc, char **argv);

static struct {
  char *name;
  int (*func)(int, char **);
} lxrc_internal[] = {
  { "sh",       util_sh_main      },
  { "lsh",      lsh_main          },
  { "insmod",   insmod_main       },
  { "rmmod",    rmmod_main        },
  { "loadkeys", loadkeys_main     },
#if WITH_PCMCIA
  { "cardmgr",  cardmgr_main      },
  { "probe",    probe_main        },
#endif
  { "setfont",  setfont_main      },
  { "portmap",  portmap_main      },
  { "mount",    util_mount_main   },
  { "umount",   util_umount_main  },
  { "cat",      util_cat_main     },
  { "echo",     util_echo_main    },
  { "ps",       util_ps_main      },
  { "nothing",  util_nothing_main }
};
#endif

int main (int argc, char **argv, char **env)
{
  char *prog;
  int i, err;

  prog = (prog = strrchr(*argv, '/')) ? prog + 1 : *argv;

#if SWISS_ARMY_KNIFE
  for(i = 0; i < sizeof lxrc_internal / sizeof *lxrc_internal; i++) {
    if(!strcmp(prog, lxrc_internal[i].name)) {
      return lxrc_internal[i].func(argc, argv);
    }
  }
#endif

  lxrc_argv = argv;

  if(!getuid()) {
    if(!util_check_exist("/oldroot")) {
      lxrc_movetotmpfs();	// does (normally) not return
    }
    else {
      // umount and release /oldroot
      umount("/oldroot");
      util_free_ramdisk("/dev/ram0");
    }
  }

  save_environment();
  lxrc_init();

  if(auto_ig) {
    err = inst_auto_install();
  }
  else if(demo_ig) {
    err = inst_start_demo();
  }
#ifdef USE_LIBHD
  else if(auto2_ig) {
    if((action_ig & ACT_RESCUE)) {
      util_manual_mode();
      util_disp_init();
      util_print_banner();
      set_choose_keytable(1);
    }
    err = inst_auto2_install();
  }
#endif
  else {
    err = 99;
  }

  if(err) {
    util_disp_init();
    lxrc_main_menu();
  }

  lxrc_end();

  return err;
}


int my_syslog (int type_iv, char *buffer_pci, ...)
    {
    va_list  args_ri;

    va_start (args_ri, buffer_pci);
    vfprintf (stderr, buffer_pci, args_ri);
    va_end (args_ri);
    fprintf (stderr, "\n");
    return (0);
    }


int my_logmessage (char *buffer_pci, ...)
    {
    va_list  args_ri;

    va_start (args_ri, buffer_pci);
    vfprintf (stderr, buffer_pci, args_ri);
    va_end (args_ri);
    fprintf (stderr, "\n");
    return (0);
    }


void lxrc_reboot (void)
    {
    if (auto_ig || auto2_ig || dia_yesno (txt_get (TXT_ASK_REBOOT), 1) == YES)
        reboot (RB_AUTOBOOT);
    }

void lxrc_halt (void)
    {
    if (auto_ig || auto2_ig || dia_yesno ("Do you want to halt the system now?", 1) == YES)
        reboot (RB_POWER_OFF);
    }

static void save_environment (void)
{
    int i;

    i = 0;
    while (environ[i++])
	;
    saved_environment = malloc (i * sizeof (char *));
    if (saved_environment)
	memcpy (saved_environment, environ, i * sizeof (char *));
}

#ifdef USE_LXRC_CHANGE_ROOT
void lxrc_change_root (void)
{
  char *new_mp;

  new_mp = (action_ig & ACT_DEMO) ? ".mnt" : "mnt";

#ifdef SYS_pivot_root
  umount ("/mnt");
  util_try_mount (lxrc_new_root, "/mnt", MS_MGC_VAL | MS_RDONLY, 0);
  chdir ("/mnt");
  if (
#if 1
      /* XXX Until glibc is fixed to provide pivot_root. */
      syscall (SYS_pivot_root, ".", new_mp)
#else
      pivot_root (".", new_mp)
#endif
      == 0)
    {
#if 0
      close (0); close (1); close (2);
      chroot (".");
      execve ("/sbin/init", lxrc_argv, saved_environment ? : environ);
#endif
      int i;

      for(i = 0; i < 20 ; i++) close(i);
      chroot(".");
      if((action_ig & ACT_DEMO)) {
        execve("/sbin/init", lxrc_argv, saved_environment ? : environ);
      }
      else {
        execl("/bin/umount", "umount", "/mnt", NULL);
      }
    }
    else {
      chdir ("/");
      umount ("/mnt");
    }
#endif
}
#endif

void lxrc_end (void)
    {
    int i;

    deb_msg("lxrc_end()");
    kill (lxrc_mempid_rm, 9);
    lxrc_killall (1);
    while(waitpid(-1, NULL, WNOHANG) == 0);
    i = waitpid(lxrc_mempid_rm, NULL, 0);
    fprintf(stderr, "lxrc_mempid_rm = %d, wait = %d\n", lxrc_mempid_rm, i);
    printf ("\033[9;15]");	/* screen saver on */
/*    reboot (RB_ENABLE_CAD); */
    mod_free_modules ();
    util_umount_driver_update ();
    (void) util_umount (mountpoint_tg);
    lxrc_set_modprobe ("/sbin/modprobe");
    lxrc_set_bdflush (40);
    (void) util_umount ("/proc/bus/usb");
    (void) util_umount ("/proc");
    disp_cursor_on ();
    kbd_end ();
    disp_end ();
#ifdef USE_LXRC_CHANGE_ROOT
    if (lxrc_new_root)
      lxrc_change_root();
#endif
    }


char *lxrc_prog_name(pid_t pid)
{
  FILE *f;
  char proc_status[64];
  static char buf[64];

  *buf = 0;
  sprintf(proc_status, "/proc/%u/status", (unsigned) pid);
  if((f = fopen(proc_status, "r"))) {
    if(fscanf(f, "Name: %30s", buf) != 1) *buf = 0;
    fclose(f);
  }

  return buf;
}

/*
 * Check if pid is either portmap or rpciod.
 *
 */
int is_rpc_prog(pid_t pid)
{
  static char *progs[] = {
    "portmap", "rpciod", "lsh"
  };
  int i;

  for(i = 0; i < sizeof progs / sizeof *progs; i++) {
    if(!strcmp(lxrc_prog_name(pid), progs[i])) return 1;
  }

  return 0;
}


/*
 * really_all = 0: kill everything except rpc progs
 * really_all = 1: kill really everything
 *
 */
void lxrc_killall (int really_all_iv)
    {
    pid_t          mypid_ri;
    struct dirent *process_pri;
    DIR           *directory_ri;
    pid_t          pid_ri;


    if (testing_ig)
        return;

    mypid_ri = getpid ();
    directory_ri = opendir ("/proc");
    if (!directory_ri)
        return;

    process_pri = readdir (directory_ri);
    while (process_pri)
        {
        pid_ri = (pid_t) atoi (process_pri->d_name);
        if (pid_ri > mypid_ri && pid_ri != lxrc_mempid_rm &&
            (really_all_iv || !is_rpc_prog (pid_ri)))
            {
            fprintf (stderr, "Killing %s (%d)\n", lxrc_prog_name (pid_ri),
                                                  pid_ri);
            kill (pid_ri, 15);
            usleep (10000);
            kill (pid_ri, 9);
            }

        process_pri = readdir (directory_ri);
        }

    (void) closedir (directory_ri);
    }


/*
 *
 * Local functions
 *
 */


#if defined(__i386__) || defined(__PPC__) || defined(__sparc__) || defined(__s390__) || defined(__s390x__)
static void lxrc_catch_signal_11(int signum, struct sigcontext scp)
#endif
#if defined(__alpha__) || defined(__ia64__)
static void lxrc_catch_signal_11(int signum, int x, struct sigcontext *scp)
#endif
{
  volatile static unsigned cnt = 0;
  uint64_t ip;

#ifdef __i386__
  ip = scp.eip;
#endif

#ifdef __PPC__
  ip = (scp.regs)->nip;
#endif

#ifdef __sparc__
  ip = scp.si_regs.pc;
#endif

#ifdef __alpha__
  ip = scp->sc_pc;
#endif

#ifdef __ia64__
  ip = scp->sc_ip;
#endif

#ifdef __s390__
  ip = (scp.sregs)->regs.psw.addr;
#endif

#ifdef __s390x__
  ip = (scp.sregs)->regs.psw.addr;
#endif

  if(++cnt < 3) fprintf(stderr, "Linuxrc segfault at 0x%08"PRIx64". :-((\n", ip);
  sleep(10);

  lxrc_sig11_im = TRUE;
}


static void lxrc_catch_signal (int sig_iv)
    {
    if (sig_iv)
        {
        fprintf (stderr, "Caught signal %d!\n", sig_iv);
        sleep (10);
        }

    if (sig_iv == SIGBUS || sig_iv == SIGSEGV)
        lxrc_sig11_im = TRUE;

    signal (SIGHUP,  lxrc_catch_signal);
    signal (SIGBUS,  lxrc_catch_signal);
    signal (SIGINT,  lxrc_catch_signal);
    signal (SIGTERM, lxrc_catch_signal);
    signal (SIGSEGV, (void (*)(int)) lxrc_catch_signal_11);
    signal (SIGPIPE, lxrc_catch_signal);
    }


static void lxrc_init (void)
    {
    int    i_ii;
    char  *language_pci;
    char  *autoyast_pci;
    char  *linuxrc_pci;
    char  *stderr_where_to;
#define LINUXRC_DEFAULT_STDERR "/dev/tty3"
    int    rc_ii;

    printf(">>> SuSE installation program v" LXRC_VERSION " (c) 1996-2001 SuSE GmbH <<<\n");
    fflush(stdout);

    if (!testing_ig && getpid () > 19)
        {
        printf ("Seems we are on a running system; activating testmode...\n");
        testing_ig = TRUE;
        strcpy (console_tg, "/dev/tty");
        }

    if (txt_init ())
        {
        printf ("Corrupted texts!\n");
        exit (-1);
        }

    siginterrupt (SIGALRM, 1);
    siginterrupt (SIGHUP,  1);
    siginterrupt (SIGBUS,  1);
    siginterrupt (SIGINT,  1);
    siginterrupt (SIGTERM, 1);
    siginterrupt (SIGSEGV, 1);
    siginterrupt (SIGPIPE, 1);
    lxrc_catch_signal (0);
/*    reboot (RB_DISABLE_CAD); */

    umask(022);

    /* just a default for manual mode */
    config.floppies = 1;
    config.floppy_dev[0] = strdup("/dev/fd0");

    language_pci = getenv ("lang");
    if(language_pci) {
      int i = set_langidbyname(language_pci);

      if(i != LANG_UNDEF) language_ig = i;
    }

#ifdef USE_LIBHD
    autoyast_pci = getenv ("autoyast");
    if (autoyast_pci)
        {
          auto2_ig = TRUE;
          yast_version_ig = 2;
	  action_ig |= ACT_YAST2_AUTO_INSTALL;
          if (!strncasecmp(autoyast_pci, "nfs:", 4)) {
            bootmode_ig = BOOTMODE_NET;
          } else if (!strncasecmp(autoyast_pci, "tftp:", 5)) {
            bootmode_ig = BOOTMODE_NET;
          } 
	}
#endif
    linuxrc_pci = getenv ("linuxrc");
    if (linuxrc_pci)
        {
        char *s = malloc (strlen (linuxrc_pci) + 3);
        if (s)
           {
           *s = 0;
           strcat (strcat (strcat (s, ","), linuxrc_pci), ",");

           if (strstr (s, ",auto,"))
               auto_ig = TRUE;

#ifdef USE_LIBHD
           if (strstr (s, ",auto2,"))
               auto2_ig = TRUE;
           if (strstr (s, ",noauto2,"))
               auto2_ig = FALSE;
           if (strstr (s, ",y2autoinst,"))
               {
               auto2_ig = TRUE;
               yast_version_ig = 2;
               action_ig |= ACT_YAST2_AUTO_INSTALL;
               }
#endif

           if (strstr (s, ",demo,"))
               {
               demo_ig = TRUE;
               action_ig |= ACT_DEMO | ACT_DEMO_LANG_SEL | ACT_LOAD_DISK;
               }

           if (strstr (s, ",eval,"))
               {
               demo_ig = TRUE;
               action_ig |= ACT_DEMO | ACT_LOAD_DISK;
               }

           if (strstr (s, ",reboot,"))
               reboot_ig = TRUE;

           if (strstr (s, ",yast1,"))
               yast_version_ig = 1;

           if (strstr (s, ",yast2,"))
               yast_version_ig = 2;

           if (strstr (s, ",loadnet,"))
               action_ig |= ACT_LOAD_NET;

           if (strstr (s, ",loaddisk,"))
               action_ig |= ACT_LOAD_DISK;

           if (strstr (s, ",french,"))
               language_ig = LANG_fr;

           if (strstr (s, ",color,"))
               color_ig = TRUE;

           if (strstr (s, ",rescue,"))
               action_ig |= ACT_RESCUE;

           if (strstr (s, ",nopcmcia,"))
               action_ig |= ACT_NO_PCMCIA;

           if (strstr (s, ",debug,"))
               action_ig |= ACT_DEBUG;

           free (s);
           }
        }

    if (!(stderr_where_to = getenv("LINUXRC_STDERR")))
      stderr_where_to = LINUXRC_DEFAULT_STDERR;

    freopen (stderr_where_to, "a", stderr);
    (void) mount ("proc", "/proc", "proc", 0, 0);
    fprintf (stderr, "Remount of / ");
    rc_ii = mount (0, "/", 0, MS_MGC_VAL | MS_REMOUNT, 0);
    fprintf (stderr, rc_ii ? "failure\n" : "success\n");

    /* Check for special case with aborted installation */

    if (util_check_exist ("/.bin"))
        {
        unlink ("/bin");
        rename ("/.bin", "/bin");
        }

    if (util_check_exist("/sbin/modprobe")) has_modprobe = 1;
    lxrc_set_modprobe ("/etc/nothing");
    lxrc_set_bdflush (5);

    lxrc_check_console ();
    freopen (console_tg, "r", stdin);
    freopen (console_tg, "a", stdout);

    util_get_splash_status();

    if(util_check_exist("/proc/iSeries")) config.is_iseries = 1;

    kbd_init ();
    util_redirect_kmsg ();
    disp_init ();
    if(splash_active) color_ig = TRUE;
    if(color_ig) disp_set_display(color_ig);

    auto2_chk_expert ();

#ifdef USE_LIBHD
    deb_int(text_mode_ig);
    deb_int(yast2_update_ig);
    deb_int(auto2_ig);
    deb_int(yast_version_ig);
    deb_int(guru_ig);

    printf ("\033[9;0]");	/* screen saver off */

    if (!auto2_ig)
#endif
        {
        for (i_ii = 1; i_ii < max_y_ig; i_ii++) printf ("\n");
        disp_cursor_off ();
        }

    info_init ();

    if (memory_ig < MEM_LIMIT_YAST2)
        yast_version_ig = 1;

    if (memory_ig > (yast_version_ig == 1 ? MEM_LIMIT_RAMDISK_YAST1 : MEM_LIMIT_RAMDISK_YAST2))
        force_ri_ig = TRUE;

    if ((guru_ig & 8)) force_ri_ig = FALSE;

#if 0
    if((action_ig & ACT_DEBUG)) {
      FILE *f = fopen("/dev/tty1", "w");
      if(f) {
        util_ps(f);
        fclose(f);
      }
      getchar();
    }
#endif

    lxrc_memcheck ();

    // #### drop this later!!!
    // if (yast_version_ig == 2) strcpy(rootimage_tg, "/suse/images/yast2");
    // deb_str(rootimage_tg);
    mod_init ();

    if(!(testing_ig || serial_ig))
      util_start_shell("/dev/tty9", "/bin/lsh", 0);

#ifdef USE_LIBHD
    if(auto2_ig) {
      if(auto2_init()) {
        auto2_ig = TRUE;
        auto2_init_settings();
      } else {
        int i, j;
        char s[200];

        deb_msg("Automatic setup not possible.");

        util_manual_mode();
        disp_cursor_off();
        disp_set_display(1);

#ifdef __i386__
        util_print_banner();
        i = 0;
        j = 1;
        if(cdrom_drives && !(action_ig & ACT_DEMO)) {
          sprintf(s, txt_get(TXT_INSERT_CD), 1);
          j = dia_okcancel(s, YES) == YES ? 1 : 0;
          if(j) {
            printf("\033c"); fflush(stdout);
            disp_clear_screen();
            i = auto2_find_install_medium();
          }
        }
        if(i) {
          auto2_ig = TRUE;
          auto2_init_settings();
        }
        else {
          yast_version_ig = 0;
          disp_cursor_off();
          util_print_banner();
          if(j) {
            char s[200];

            sprintf(s,
              "Could not find the SuSE Linux %s CD.\n\nActivating manual setup program.\n",
              (action_ig & ACT_DEMO) ? "LiveEval" : "Installation"
            );
            dia_message(s, MSGTYPE_ERROR);
          }
        }
#endif

      }
    }
#endif

    util_print_banner ();

    /* note: for auto2, file_read_info() is called inside auto2_init() */
    if (auto_ig) 
        {
	rename_info_file ();
        file_read_info ();
        }
	
    if((!auto2_ig && language_ig == LANG_UNDEF) || (auto2_ig && demo_ig)) {
      set_choose_language ();
    }

    util_print_banner ();

    deb_int(color_ig);

    if (!color_ig)	/* if it hasn't already been set */
        {
        set_choose_display ();
        util_print_banner ();
        }

    if (!(serial_ig || config.is_iseries))
        set_choose_keytable (0);

    util_update_kernellog ();

    (void) net_setup_localhost ();

#if !defined(__PPC__) && !defined(__sparc__)
    if(!auto_ig || reboot_wait_ig) {
      config.rebootmsg = 1;
    }
#endif

    }


static int lxrc_main_cb (int what_iv)
    {
    int    rc_ii = 1;


    switch (what_iv)
        {
        case 1:
            rc_ii = set_settings ();
            if (rc_ii == 1 || rc_ii == 2)
                rc_ii = 0;
            else if (rc_ii == 0)
                rc_ii = 1;
            break;
        case 2:
            info_menu ();
            break;
        case 3:
            mod_menu ();
            break;
        case 4:
            rc_ii = inst_menu ();
            break;
        case 5:
            lxrc_reboot ();
            break;
        case 6:
            lxrc_halt ();
            break;
        default:
            break;
        }

    return (rc_ii);
    }


static void lxrc_main_menu (void)
    {
    int    width_ii = 40;
    item_t items_ari [6];
    int    i_ii;
    int    choice_ii;
    int    nr_items_ii = sizeof (items_ari) / sizeof (items_ari [0]);

    util_manual_mode();

    util_create_items (items_ari, nr_items_ii, width_ii);

    do
        {
        strcpy (items_ari [0].text, txt_get (TXT_SETTINGS));
        strcpy (items_ari [1].text, txt_get (TXT_MENU_INFO));
        strcpy (items_ari [2].text, txt_get (TXT_MENU_MODULES));
        strcpy (items_ari [3].text, txt_get (TXT_MENU_START));
        strcpy (items_ari [4].text, txt_get (TXT_END_REBOOT));
        strcpy (items_ari [5].text, "Power off");
        for (i_ii = 0; i_ii < nr_items_ii; i_ii++)
            {
            util_center_text (items_ari [i_ii].text, width_ii);
            items_ari [i_ii].func = lxrc_main_cb;
            }

        choice_ii = dia_menu (txt_get (TXT_HDR_MAIN), items_ari,
                              nr_items_ii, 4);
        if (!choice_ii)
            if (dia_message (txt_get (TXT_NO_LXRC_END), MSGTYPE_INFO) == -42)
                choice_ii = 42;

        if (choice_ii == 1)
            {
            util_print_banner ();
            choice_ii = 0;
            }
        }
    while (!choice_ii);

    util_free_items (items_ari, nr_items_ii);
    }


static void lxrc_memcheck (void)
    {
    int   nr_pages_ii;
    int   max_pages_ii;
    char *data_pci;
    int   i_ii;


    lxrc_mempid_rm = fork ();
    if (lxrc_mempid_rm)
        return;

    lxrc_catch_signal (0);

    if (memory_ig < 8000000)
        max_pages_ii = 40;
    else
        max_pages_ii = 100;

    while (1)
        {
        nr_pages_ii = 1 + (int) ((double) max_pages_ii * rand () / (RAND_MAX + 1.0));
        data_pci = malloc (nr_pages_ii * 4096);
        if (data_pci)
            {
            for (i_ii = 0; i_ii < nr_pages_ii && !lxrc_sig11_im; i_ii++)
                {
                data_pci [i_ii * 4096] = 42;
                usleep (10000);
                }
            lxrc_sig11_im = FALSE;
            free (data_pci);
            }
        sleep (1);
        }
    }


void lxrc_set_modprobe (char *program_tv)
    {
    FILE  *proc_file_pri;

    /* do nothing if we have a modprobe */
    if (has_modprobe) return;

    proc_file_pri = fopen ("/proc/sys/kernel/modprobe", "w");
    if (proc_file_pri)
        {
        fprintf (proc_file_pri, "%s\n", program_tv);
        fclose (proc_file_pri);
        }
    }


/* Check if we start linuxrc on a serial console. On Intel and
   Alpha, we look if the "console" parameter was used on the
   commandline. On SPARC, we use the result from hardwareprobing. */
static void lxrc_check_console (void)
    {
#if !defined(__sparc__) && !defined(__PPC__)
    FILE  *fd_pri;
    char   buffer_ti [300];
    char  *tmp_pci = NULL;
    char  *found_pci = NULL;

    fd_pri = fopen ("/proc/cmdline", "r");
    if (!fd_pri)
        return;

    if (fgets (buffer_ti, sizeof buffer_ti - 1, fd_pri))
        {
        tmp_pci = strstr (buffer_ti, "console");
        while (tmp_pci)
            {
            found_pci = tmp_pci;
            tmp_pci = strstr (found_pci + 1, "console");
            }
        }

    if (found_pci)
        {
        while (*found_pci && *found_pci != '=')
            found_pci++;

        found_pci++;

	/* Find the whole console= entry for the install.inf file */
        tmp_pci = found_pci;
        while (*tmp_pci && !isspace(*tmp_pci)) tmp_pci++;
        *tmp_pci = 0;

	strncpy (console_parms_tg, found_pci, sizeof console_parms_tg);
	console_parms_tg[sizeof console_parms_tg - 1] = '\0';

	/* Now search only for the device name */
	tmp_pci = found_pci;
        while (*tmp_pci && *tmp_pci != ',') tmp_pci++;
        *tmp_pci = 0;

        sprintf (console_tg, "/dev/%s", found_pci);
        if (!strncmp (found_pci, "ttyS", 4)) {
            serial_ig = TRUE;
            text_mode_ig = TRUE;
          }

        fprintf (stderr, "Console: %s, serial=%d\n", console_tg, serial_ig);
        }

    fclose (fd_pri);
#else
#ifdef USE_LIBHD
    char *cp;

    cp = auto2_serial_console ();
    if (cp && strlen (cp) > 0)
      {
	serial_ig = TRUE;
        text_mode_ig = TRUE;

	strcpy (console_tg, cp);

        fprintf (stderr, "Console: %s, serial=%d\n", console_tg, serial_ig);
      }
#endif
#endif
    }


static void lxrc_set_bdflush (int percent_iv)
    {
    FILE  *fd_pri;


    fd_pri = fopen ("/proc/sys/vm/bdflush", "w");
    if (!fd_pri)
        return;

    fprintf (fd_pri, "%d", percent_iv);
    fclose (fd_pri);
    }


/*
 * Copy root tree into a tmpfs tree, pivot_root() there and
 * exec() the new linuxrc.
 */
void lxrc_movetotmpfs()
{
  int i;
  char *newroot = "/newroot";

  fprintf(stderr, "Moving into tmpfs...");
  i = mkdir(newroot, 0755);
  if(i) {
    perror(newroot);
    return;
  }

  i = mount("shmfs", newroot, "shm", 0, 0);
  if(i) {
    perror(newroot);
    return;
  }

  i = util_do_cp("/", newroot);
  if(i) {
    fprintf(stderr, "copy failed: %d\n", i);
    return;
  }

  fprintf(stderr, " done.\n");

  i = chdir(newroot);
  if(i) {
    perror(newroot);
    return;
  }

  i = mkdir("oldroot", 0755);
  if(i) {
    perror("oldroot");
    return;
  }

  if(
#if 1
    !syscall(SYS_pivot_root, ".", "oldroot")
#else
    !pivot_root(".", "oldroot")
#endif
  ) {
    for(i = 0; i < 20; i++) close(i);
    chroot(".");
    open("/dev/console", O_RDWR);
    dup(0);
    dup(0);
    execve("/linuxrc", lxrc_argv, environ);
    perror("/linuxrc");
  }
  else {
    fprintf(stderr, "Oops, pivot_root failed\n");
  }
}

