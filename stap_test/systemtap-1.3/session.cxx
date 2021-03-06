// session functions
// Copyright (C) 2010 Red Hat Inc.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "session.h"
#include "cache.h"
#include "elaborate.h"
#include "translate.h"
#include "buildrun.h"
#include "coveragedb.h"
#include "hash.h"
#include "task_finder.h"
#include "csclient.h"
#include "rpm_finder.h"
#include "util.h"
#include "git_version.h"

#include "sys/sdt.h"

#include <cerrno>
#include <cstdlib>

extern "C" {
#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <elfutils/libdwfl.h>
}

#include <string>

using namespace std;

/* getopt variables */
extern int optind;

#define PATH_TBD string("__TBD__")

systemtap_session::systemtap_session ():
  // NB: pointer members must be manually initialized!
  base_hash(0),
  pattern_root(new match_node),
  user_file (0),
  be_derived_probes(0),
  dwarf_derived_probes(0),
  kprobe_derived_probes(0),
  hwbkpt_derived_probes(0),
  perf_derived_probes(0),
  uprobe_derived_probes(0),
  utrace_derived_probes(0),
  itrace_derived_probes(0),
  task_finder_derived_probes(0),
  timer_derived_probes(0),
  profile_derived_probes(0),
  mark_derived_probes(0),
  tracepoint_derived_probes(0),
  hrtimer_derived_probes(0),
  procfs_derived_probes(0),
  op (0), up (0),
  sym_kprobes_text_start (0),
  sym_kprobes_text_end (0),
  sym_stext (0),
  module_cache (0),
  last_token (0)
{
  struct utsname buf;
  (void) uname (& buf);
  kernel_release = string (buf.release);
  release = kernel_release;
  kernel_build_tree = "/lib/modules/" + kernel_release + "/build";

  // PR4186: Copy logic from coreutils uname (uname -i) to squash
  // i?86->i386.  Actually, copy logic from linux top-level Makefile
  // to squash uname -m -> $(SUBARCH).

  machine = buf.machine;
  if (machine == "i486") machine = "i386";
  else if (machine == "i586") machine = "i386";
  else if (machine == "i686") machine = "i386";
  else if (machine == "sun4u") machine = "sparc64";
  else if (machine.substr(0,3) == "arm") machine = "arm";
  else if (machine == "sa110") machine = "arm";
  else if (machine == "s390x") machine = "s390";
  else if (machine.substr(0,3) == "ppc") machine = "powerpc";
  else if (machine.substr(0,4) == "mips") machine = "mips";
  else if (machine.substr(0,3) == "sh2") machine = "sh";
  else if (machine.substr(0,3) == "sh3") machine = "sh";
  else if (machine.substr(0,3) == "sh4") machine = "sh";
  architecture = machine;

  for (unsigned i=0; i<5; i++) perpass_verbose[i]=0;
  verbose = 0;

  have_script = false;
  runtime_specified = false;
  include_arg_start = -1;
  timing = false;
  guru_mode = false;
  bulk_mode = false;
  unoptimized = false;
  suppress_warnings = false;
  panic_warnings = false;
  listing_mode = false;
  listing_mode_vars = false;

#ifdef ENABLE_PROLOGUES
  prologue_searching = true;
#else
  prologue_searching = false;
#endif

  buffer_size = 0;
  last_pass = 5;
  module_name = "stap_" + lex_cast(getpid());
  stapconf_name = "stapconf_" + lex_cast(getpid()) + ".h";
  output_file = ""; // -o FILE
  save_module = false;
  keep_tmpdir = false;
  cmd = "";
  target_pid = 0;
  use_cache = true;
  use_script_cache = true;
  poison_cache = false;
  tapset_compile_coverage = false;
  need_uprobes = false;
  consult_symtab = false;
  ignore_vmlinux = false;
  ignore_dwarf = false;
  load_only = false;
  skip_badvars = false;
  unprivileged = false;
  omit_werror = false;
  compatible = VERSION; // XXX: perhaps also process GIT_SHAID if available?
  unwindsym_ldd = false;
  client_options = false;

  /*  adding in the XDG_DATA_DIRS variable path,
   *  this searches in conjunction with SYSTEMTAP_TAPSET
   *  to locate stap scripts, either can be disabled if 
   *  needed using env $PATH=/dev/null where $PATH is the 
   *  path you want disabled
   */  
  const char* s_p1 = getenv ("XDG_DATA_DIRS");
  if ( s_p1 != NULL )
  {
    vector<string> dirs;
    tokenize(s_p1, dirs, ":");
    for(vector<string>::iterator i = dirs.begin(); i != dirs.end(); ++i)
    {
      include_path.push_back(*i + "/systemtap/tapset");
    }
  }

  const char* s_p = getenv ("SYSTEMTAP_TAPSET");
  if (s_p != NULL)
  {
    include_path.push_back (s_p);
  }
  else
  {
    include_path.push_back (string(PKGDATADIR) + "/tapset");
  }

  const char* s_r = getenv ("SYSTEMTAP_RUNTIME");
  if (s_r != NULL)
    runtime_path = s_r;
  else
    runtime_path = string(PKGDATADIR) + "/runtime";

  const char* s_d = getenv ("SYSTEMTAP_DIR");
  if (s_d != NULL)
    data_path = s_d;
  else
    data_path = get_home_directory() + string("/.systemtap");
  if (create_dir(data_path.c_str()) == 1)
    {
      const char* e = strerror (errno);
      if (! suppress_warnings)
        cerr << "Warning: failed to create systemtap data directory (\""
             << data_path << "\"): " << e
             << ", disabling cache support." << endl;
      use_cache = use_script_cache = false;
    }

  if (use_cache)
    {
      cache_path = data_path + "/cache";
      if (create_dir(cache_path.c_str()) == 1)
        {
	  const char* e = strerror (errno);
          if (! suppress_warnings)
            cerr << "Warning: failed to create cache directory (\""
                 << cache_path << "\"): " << e
                 << ", disabling cache support." << endl;
	  use_cache = use_script_cache = false;
	}
    }

  const char* s_tc = getenv ("SYSTEMTAP_COVERAGE");
  if (s_tc != NULL)
    tapset_compile_coverage = true;

  const char* s_kr = getenv ("SYSTEMTAP_RELEASE");
  if (s_kr != NULL) {
    setup_kernel_release(s_kr);
  }
}


void
systemtap_session::version ()
{
  clog
    << "SystemTap translator/driver "
    << "(version " << VERSION << "/" << dwfl_version (NULL)
    << " " << GIT_MESSAGE << ")" << endl
    << "Copyright (C) 2005-2010 Red Hat, Inc. and others" << endl
    << "This is free software; see the source for copying conditions." << endl;
  clog << "enabled features:"
#ifdef HAVE_AVAHI
       << " AVAHI"
#endif
#ifdef HAVE_LIBRPM
       << " LIBRPM"
#endif
#ifdef HAVE_LIBSQLITE3
       << " LIBSQLITE3"
#endif
#ifdef HAVE_NSS
       << " NSS"
#endif
#ifdef HAVE_BOOST_SHARED_PTR_HPP
       << " BOOST_SHARED_PTR"
#endif
#ifdef HAVE_TR1_UNORDERED_MAP
       << " TR1_UNORDERED_MAP"
#endif
#ifdef ENABLE_PROLOGUES
       << " PROLOGUES"
#endif
       << endl;
}

void
systemtap_session::usage (int exitcode)
{
  version ();
  clog
    << endl
    << "Usage: stap [options] FILE         Run script in file."
    << endl
    << "   or: stap [options] -            Run script on stdin."
    << endl
    << "   or: stap [options] -e SCRIPT    Run given script."
    << endl
    << "   or: stap [options] -l PROBE     List matching probes."
    << endl
    << "   or: stap [options] -L PROBE     List matching probes and local variables."
    << endl
    << endl
    << "Options:" << endl
    << "   --         end of translator options, script options follow" << endl
    << "   -h --help  show help" << endl
    << "   -V         show version" << endl
    << "   -p NUM     stop after pass NUM 1-5, instead of " << last_pass << endl
    << "              (parse, elaborate, translate, compile, run)" << endl
    << "   -v         add verbosity to all passes" << endl
    << "   --vp {N}+  add per-pass verbosity [";
  for (unsigned i=0; i<5; i++)
    clog << (perpass_verbose[i] <= 9 ? perpass_verbose[i] : 9);
  clog 
    << "]" << endl
    << "   -k         keep temporary directory" << endl
    << "   -u         unoptimized translation" << (unoptimized ? " [set]" : "") << endl
    << "   -w         suppress warnings" << (suppress_warnings ? " [set]" : "") << endl
    << "   -W         turn warnings into errors" << (panic_warnings ? " [set]" : "") << endl
    << "   -g         guru mode" << (guru_mode ? " [set]" : "") << endl
    << "   -P         prologue-searching for function probes"
    << (prologue_searching ? " [set]" : "") << endl
    << "   -b         bulk (percpu file) mode" << (bulk_mode ? " [set]" : "") << endl
    << "   -s NUM     buffer size in megabytes, instead of " << buffer_size << endl
    << "   -I DIR     look in DIR for additional .stp script files";
  if (include_path.size() == 0)
    clog << endl;
  else
    clog << ", in addition to" << endl;
  for (unsigned i=0; i<include_path.size(); i++)
    clog << "              " << include_path[i] << endl;
  clog
    << "   -D NM=VAL  emit macro definition into generated C code" << endl
    << "   -B NM=VAL  pass option to kbuild make" << endl
    << "   -G VAR=VAL set global variable to value" << endl
    << "   -R DIR     look in DIR for runtime, instead of" << endl
    << "              " << runtime_path << endl
    << "   -r DIR     cross-compile to kernel with given build tree; or else" << endl
    << "   -r RELEASE cross-compile to kernel /lib/modules/RELEASE/build, instead of" << endl
    << "              " << kernel_build_tree << endl
    << "   -a ARCH    cross-compile to given architecture, instead of " << architecture << endl
    << "   -m MODULE  set probe module name, instead of " << endl
    << "              " << module_name << endl
    << "   -o FILE    send script output to file, instead of stdout. This supports" << endl
    << "              strftime(3) formats for FILE" << endl
    << "   -c CMD     start the probes, run CMD, and exit when it finishes" << endl
    << "   -x PID     sets target() to PID" << endl
    << "   -F         run as on-file flight recorder with -o." << endl
    << "              run as on-memory flight recorder without -o." << endl
    << "   -S size[,n] set maximum of the size and the number of files." << endl
    << "   -d OBJECT  add unwind/symbol data for OBJECT file";
  if (unwindsym_modules.size() == 0)
    clog << endl;
  else
    clog << ", in addition to" << endl;
  {
    vector<string> syms (unwindsym_modules.begin(), unwindsym_modules.end());
    for (unsigned i=0; i<syms.size(); i++)
      clog << "              " << syms[i] << endl;
  }
  clog
    << "   --ldd      add unwind/symbol data for all referenced OBJECT files." << endl
    << "   --all-modules" << endl
    << "              add unwind/symbol data for all loaded kernel objects." << endl
    << "   -t         collect probe timing information" << endl
#ifdef HAVE_LIBSQLITE3
    << "   -q         generate information on tapset coverage" << endl
#endif /* HAVE_LIBSQLITE3 */
    << "   --unprivileged" << endl
    << "              restrict usage to features available to unprivileged users" << endl
#if 0 /* PR6864: disable temporarily; should merge with -d somehow */
    << "   --kelf     make do with symbol table from vmlinux" << endl
    << "   --kmap[=FILE]" << endl
    << "              make do with symbol table from nm listing" << endl
#endif
  // Formerly present --ignore-{vmlinux,dwarf} options are for testsuite use
  // only, and don't belong in the eyesight of a plain user.
    << "   --compatible=VERSION" << endl
    << "              suppress incompatible language/tapset changes beyond VERSION," << endl
    << "              instead of " << compatible << endl
    << "   --skip-badvars" << endl
    << "              substitute zero for bad context $variables" << endl
#if HAVE_NSS
    << "   --use-server[=SERVER-SPEC]" << endl
    << "              compile using a systemtap compile-server" << endl
#endif
#if HAVE_NSS || HAVE_AVAHI
    << "   --list-servers[=PROPERTIES]" << endl
    << "              report on the status of the specified compile-servers" << endl
#endif
    << endl
    ;

  time_t now;
  time (& now);
  struct tm* t = localtime (& now);
  if (t && t->tm_mon*3 + t->tm_mday*173 == 0xb6)
    clog << morehelp << endl;

  exit (exitcode);
}

int
systemtap_session::parse_cmdline (int argc, char * const argv [])
{
  client_options_disallowed = "";
  while (true)
    {
      int long_opt;
      char * num_endptr;

      // NB: when adding new options, consider very carefully whether they
      // should be restricted from stap-clients (after --client-options)!
#define LONG_OPT_KELF 1
#define LONG_OPT_KMAP 2
#define LONG_OPT_IGNORE_VMLINUX 3
#define LONG_OPT_IGNORE_DWARF 4
#define LONG_OPT_VERBOSE_PASS 5
#define LONG_OPT_SKIP_BADVARS 6
#define LONG_OPT_UNPRIVILEGED 7
#define LONG_OPT_OMIT_WERROR 8
#define LONG_OPT_CLIENT_OPTIONS 9
#define LONG_OPT_HELP 10
#define LONG_OPT_DISABLE_CACHE 11
#define LONG_OPT_POISON_CACHE 12
#define LONG_OPT_CLEAN_CACHE 13
#define LONG_OPT_COMPATIBLE 14
#define LONG_OPT_LDD 15
#define LONG_OPT_USE_SERVER 16
#define LONG_OPT_LIST_SERVERS 17
#define LONG_OPT_ALL_MODULES 18
      // NB: also see find_hash(), usage(), switch stmt below, stap.1 man page
      static struct option long_options[] = {
        { "kelf", 0, &long_opt, LONG_OPT_KELF },
        { "kmap", 2, &long_opt, LONG_OPT_KMAP },
        { "ignore-vmlinux", 0, &long_opt, LONG_OPT_IGNORE_VMLINUX },
        { "ignore-dwarf", 0, &long_opt, LONG_OPT_IGNORE_DWARF },
	{ "skip-badvars", 0, &long_opt, LONG_OPT_SKIP_BADVARS },
        { "vp", 1, &long_opt, LONG_OPT_VERBOSE_PASS },
        { "unprivileged", 0, &long_opt, LONG_OPT_UNPRIVILEGED },
#define OWE5 "tter"
#define OWE1 "uild-"
#define OWE6 "fu-kb"
#define OWE2 "i-kno"
#define OWE4 "st"
#define OWE3 "w-be"
        { OWE4 OWE6 OWE1 OWE2 OWE3 OWE5, 0, &long_opt, LONG_OPT_OMIT_WERROR },
        { "client-options", 0, &long_opt, LONG_OPT_CLIENT_OPTIONS },
        { "help", 0, &long_opt, LONG_OPT_HELP },
        { "disable-cache", 0, &long_opt, LONG_OPT_DISABLE_CACHE },
        { "poison-cache", 0, &long_opt, LONG_OPT_POISON_CACHE },
        { "clean-cache", 0, &long_opt, LONG_OPT_CLEAN_CACHE },
        { "compatible", 1, &long_opt, LONG_OPT_COMPATIBLE },
        { "ldd", 0, &long_opt, LONG_OPT_LDD },
        { "use-server", 2, &long_opt, LONG_OPT_USE_SERVER },
        { "list-servers", 2, &long_opt, LONG_OPT_LIST_SERVERS },
        { "all-modules", 0, &long_opt, LONG_OPT_ALL_MODULES },
        { NULL, 0, NULL, 0 }
      };
      int grc = getopt_long (argc, argv, "hVvtp:I:e:o:R:r:a:m:kgPc:x:D:bs:uqwl:d:L:FS:B:WG:",
			     long_options, NULL);
      // NB: when adding new options, consider very carefully whether they
      // should be restricted from stap-clients (after --client-options)!

      if (grc < 0)
        break;
      bool push_server_opt = false;
      switch (grc)
        {
        case 'V':
	  push_server_opt = true;
          version ();
          exit (0);

        case 'v':
	  push_server_opt = true;
          for (unsigned i=0; i<5; i++)
            perpass_verbose[i] ++;
	  verbose ++;
	  break;

        case 'G':
          // Make sure the global option is only composed of the
          // following chars: [_=a-zA-Z0-9]
          assert_regexp_match("-G parameter", optarg, "^[a-z_][a-z0-9_]+=[a-z0-9_-]+$");

          globalopts.push_back (string(optarg));
          break;

        case 't':
	  push_server_opt = true;
	  timing = true;
	  break;

        case 'w':
	  push_server_opt = true;
	  suppress_warnings = true;
	  break;

        case 'W':
	  push_server_opt = true;
	  panic_warnings = true;
	  break;

        case 'p':
          last_pass = (int)strtoul(optarg, &num_endptr, 10);
          if (*num_endptr != '\0' || last_pass < 1 || last_pass > 5)
            {
              cerr << "Invalid pass number (should be 1-5)." << endl;
              return 1;
            }
          if (listing_mode && last_pass != 2)
            {
              cerr << "Listing (-l) mode implies pass 2." << endl;
              return 1;
            }
	  push_server_opt = true;
          break;

        case 'I':
	  if (client_options)
	    client_options_disallowed += client_options_disallowed.empty () ? "-I" : ", -I";
	  if (include_arg_start == -1)
	    include_arg_start = include_path.size ();
          include_path.push_back (string (optarg));
          break;

        case 'd':
	  push_server_opt = true;
          {
            // At runtime user module names are resolved through their
            // canonical (absolute) path.
            const char *mpath = canonicalize_file_name (optarg);
            if (mpath == NULL) // Must be a kernel module name
              mpath = optarg;
            unwindsym_modules.insert (string (mpath));
            // PR10228: trigger vma tracker logic early if -d /USER-MODULE/
            // given. XXX This is actually too early. Having a user module
            // is a good indicator that something will need vma tracking.
            // But it is not 100%, this really should only trigger through
            // a user mode tapset /* pragma:vma */ or a probe doing a
            // variable lookup through a dynamic module.
            if (mpath[0] == '/')
              enable_vma_tracker (*this);
            break;
          }

        case 'e':
	  if (have_script)
	    {
	      cerr << "Only one script can be given on the command line."
		   << endl;
              return 1;
	    }
	  push_server_opt = true;
          cmdline_script = string (optarg);
          have_script = true;
          break;

        case 'o':
          // NB: client_options not a problem, since pass 1-4 does not use output_file.
	  push_server_opt = true;
          output_file = string (optarg);
          break;

        case 'R':
          if (client_options) { cerr << "ERROR: -R invalid with --client-options" << endl; return 1; }
	  runtime_specified = true;
          runtime_path = string (optarg);
          break;

        case 'm':
	  if (client_options)
	    client_options_disallowed += client_options_disallowed.empty () ? "-m" : ", -m";
          module_name = string (optarg);
	  save_module = true;
	  {
	    // If the module name ends with '.ko', chop it off since
	    // modutils doesn't like modules named 'foo.ko.ko'.
	    if (endswith(module_name, ".ko"))
	      {
		module_name.erase(module_name.size() - 3);
		cerr << "Truncating module name to '" << module_name
		     << "'" << endl;
	      }

	    // Make sure an empty module name wasn't specified (-m "")
	    if (module_name.empty())
	    {
		cerr << "Module name cannot be empty." << endl;
		return 1;
	    }

	    // Make sure the module name is only composed of the
	    // following chars: [a-z0-9_]
            assert_regexp_match("-m parameter", module_name, "^[a-z0-9_]+$");

	    // Make sure module name isn't too long.
	    if (module_name.size() >= (MODULE_NAME_LEN - 1))
	      {
		module_name.resize(MODULE_NAME_LEN - 1);
		cerr << "Truncating module name to '" << module_name
		     << "'" << endl;
	      }
	  }

	  push_server_opt = true;
	  use_script_cache = false;
          break;

        case 'r':
          if (client_options) // NB: no paths!
            assert_regexp_match("-r parameter from client", optarg, "^[a-z0-9_.-]+$");
	  push_server_opt = true;
          setup_kernel_release(optarg);
          break;

        case 'a':
          assert_regexp_match("-a parameter", optarg, "^[a-z0-9_-]+$");
	  push_server_opt = true;
          architecture = string(optarg);
          break;

        case 'k':
	  push_server_opt = true;
          keep_tmpdir = true;
          use_script_cache = false; /* User wants to keep a usable build tree. */
          break;

        case 'g':
	  push_server_opt = true;
          guru_mode = true;
          break;

        case 'P':
	  push_server_opt = true;
          prologue_searching = true;
          break;

        case 'b':
	  push_server_opt = true;
          bulk_mode = true;
          break;

	case 'u':
	  push_server_opt = true;
	  unoptimized = true;
	  break;

        case 's':
          buffer_size = (int) strtoul (optarg, &num_endptr, 10);
          if (*num_endptr != '\0' || buffer_size < 1 || buffer_size > 4095)
            {
              cerr << "Invalid buffer size (should be 1-4095)." << endl;
	      return 1;
            }
	  push_server_opt = true;
          break;

	case 'c':
	  push_server_opt = true;
	  cmd = string (optarg);
	  break;

	case 'x':
	  target_pid = (int) strtoul(optarg, &num_endptr, 10);
	  if (*num_endptr != '\0')
	    {
	      cerr << "Invalid target process ID number." << endl;
	      return 1;
	    }
	  push_server_opt = true;
	  break;

	case 'D':
          assert_regexp_match ("-D parameter", optarg, "^[a-z_][a-z_0-9]*(=-?[a-z_0-9]+)?$");
	  if (client_options)
	    client_options_disallowed += client_options_disallowed.empty () ? "-D" : ", -D";
	  push_server_opt = true;
	  macros.push_back (string (optarg));
	  break;

	case 'S':
          assert_regexp_match ("-S parameter", optarg, "^[0-9]+(,[0-9]+)?$");
	  push_server_opt = true;
	  size_option = string (optarg);
	  break;

	case 'q':
          if (client_options) { cerr << "ERROR: -q invalid with --client-options" << endl; return 1; } 
	  push_server_opt = true;
	  tapset_compile_coverage = true;
	  break;

        case 'h':
          usage (0);
          break;

        case 'L':
          listing_mode_vars = true;
          unoptimized = true; // This causes retention of variables for listing_mode
          // fallthrough
        case 'l':
	  suppress_warnings = true;
          listing_mode = true;
          last_pass = 2;
          if (have_script)
            {
	      cerr << "Only one script can be given on the command line."
		   << endl;
	      return 1;
            }
	  push_server_opt = true;
          cmdline_script = string("probe ") + string(optarg) + " {}";
          have_script = true;
          break;

        case 'F':
	  push_server_opt = true;
          load_only = true;
	  break;

	case 'B':
          if (client_options) { cerr << "ERROR: -B invalid with --client-options" << endl; return 1; } 
	  push_server_opt = true;
          kbuildflags.push_back (string (optarg));
	  break;

        case 0:
          switch (long_opt)
            {
            case LONG_OPT_KELF:
	      push_server_opt = true;
	      consult_symtab = true;
	      break;
            case LONG_OPT_KMAP:
	      // Leave consult_symtab unset for now, to ease error checking.
              if (!kernel_symtab_path.empty())
		{
		  cerr << "You can't specify multiple --kmap options." << endl;
		  return 1;
		}
	      push_server_opt = true;
              if (optarg)
		kernel_symtab_path = optarg;
              else
                kernel_symtab_path = PATH_TBD;
	      break;
	    case LONG_OPT_IGNORE_VMLINUX:
	      push_server_opt = true;
	      ignore_vmlinux = true;
	      break;
	    case LONG_OPT_IGNORE_DWARF:
	      push_server_opt = true;
	      ignore_dwarf = true;
	      break;
	    case LONG_OPT_VERBOSE_PASS:
              {
                bool ok = true;
                if (strlen(optarg) < 1 || strlen(optarg) > 5)
                  ok = false;
                if (ok)
                  for (unsigned i=0; i<strlen(optarg); i++)
                    if (isdigit (optarg[i]))
                      perpass_verbose[i] += optarg[i]-'0';
                    else
                      ok = false;
                
                if (! ok)
                  {
                    cerr << "Invalid --vp argument: it takes 1 to 5 digits." << endl;
                    return 1;
                  }
                // NB: we don't do this: last_pass = strlen(optarg);
		push_server_opt = true;
                break;
              }
	    case LONG_OPT_SKIP_BADVARS:
	      push_server_opt = true;
	      skip_badvars = true;
	      break;
	    case LONG_OPT_UNPRIVILEGED:
	      push_server_opt = true;
	      unprivileged = true;
              /* NB: for server security, it is essential that once this flag is
                 set, no future flag be able to unset it. */
	      break;
	    case LONG_OPT_OMIT_WERROR:
	      push_server_opt = true;
	      omit_werror = true;
	      break;
	    case LONG_OPT_CLIENT_OPTIONS:
	      client_options = true;
	      break;
	    case LONG_OPT_USE_SERVER:
	      if (client_options)
		client_options_disallowed += client_options_disallowed.empty () ? "--use-server" : ", --use-server";
	      if (optarg)
		specified_servers.push_back (optarg);
	      else
		specified_servers.push_back ("");
	      break;
	    case LONG_OPT_LIST_SERVERS:
	      if (client_options)
		client_options_disallowed += client_options_disallowed.empty () ? "--list-servers" : ", --list-servers";
	      if (optarg )
		server_status_strings.push_back (optarg);
	      else
		server_status_strings.push_back ("");
	      break;
	    case LONG_OPT_HELP:
	      usage (0);
	      break;

            // The caching options should not be available to server clients
            case LONG_OPT_DISABLE_CACHE:
              if (client_options) {
                  cerr << "ERROR: --disable-cache is invalid with --client-options" << endl;
                  return 1;
              }
              use_cache = use_script_cache = false;
              break;
            case LONG_OPT_POISON_CACHE:
              if (client_options) {
                  cerr << "ERROR: --poison-cache is invalid with --client-options" << endl;
                  return 1;
              }
              poison_cache = true;
              break;
            case LONG_OPT_CLEAN_CACHE:
              if (client_options) {
                  cerr << "ERROR: --clean-cache is invalid with --client-options" << endl;
                  return 1;
              }
              clean_cache(*this);
              exit(0);

            case LONG_OPT_COMPATIBLE:
	      push_server_opt = true;
              compatible = optarg;
              break;

            case LONG_OPT_LDD:
              if (client_options) {
                  cerr << "ERROR: --ldd is invalid with --client-options" << endl;
                  return 1;
              }
	      push_server_opt = true;
              unwindsym_ldd = true;
              break;

            case LONG_OPT_ALL_MODULES:
              if (client_options) {
                  cerr << "ERROR: --all-modules is invalid with --client-options" << endl;
                  return 1;
              }
              insert_loaded_modules();
              break;

            default:
              // NOTREACHED unless one added a getopt option but not a corresponding switch/case:
              cerr << "Unhandled long argument id " << long_opt << endl;
              return 1;
            }
          break;

	case '?':
	  // Invalid/unrecognized option given or argument required, but
	  // not given. In both cases getopt_long() will have printed the
	  // appropriate error message to stderr already.
	  return 1;
	  break;

        default:
          // NOTREACHED unless one added a getopt option but not a corresponding switch/case:
          cerr << "Unhandled argument code " << (char)grc << endl;
          return 1;
          break;
        }

      // Pass selected options on to the server, if any.
      if (push_server_opt && ! specified_servers.empty ())
	{
	  if (grc == 0)
	    server_args.push_back (string ("--") +
				   long_options[long_opt - 1].name);
	  else
	    server_args.push_back (string ("-") + (char)grc);
	  if (optarg)
	    server_args.push_back (optarg);
	}
    }

  return 0;
}

void
systemtap_session::check_options (int argc, char * const argv [])
{
#if ! HAVE_NSS
  if (client_options)
    cerr << "WARNING: --client-options is not supported by this version of systemtap" << endl;
  if (! specified_servers.empty ())
    {
      cerr << "WARNING: --use-server is not supported by this version of systemtap" << endl;
      specified_servers.clear ();
    }
#endif

#if ! HAVE_NSS && ! HAVE_AVAHI
  if (! server_status_strings.empty ())
    {
      cerr << "WARNING: --list-servers is not supported by this version of systemtap" << endl;
      server_status_strings.clear ();
    }
#endif

  if (runtime_specified && ! specified_servers.empty ())
    {
      cerr << "Warning: Ignoring --use-server due to the use of -R" << endl;
      specified_servers.clear ();
    }

  if (client_options && last_pass > 4)
    {
      last_pass = 4; /* Quietly downgrade.  Server passed through -p5 naively. */
    }
  if (client_options && unprivileged && ! client_options_disallowed.empty ())
    {
      cerr << "You can't specify " << client_options_disallowed << " when --unprivileged is specified." << endl;
      usage (1);
    }
  if ((cmd != "") && (target_pid))
    {
      cerr << "You can't specify -c and -x options together." << endl;
      usage (1);
    }
  if (unprivileged && guru_mode)
    {
      cerr << "You can't specify -g and --unprivileged together." << endl;
      usage (1);
    }
  if (!kernel_symtab_path.empty())
    {
      if (consult_symtab)
      {
        cerr << "You can't specify --kelf and --kmap together." << endl;
        usage (1);
      }
      consult_symtab = true;
      if (kernel_symtab_path == PATH_TBD)
        kernel_symtab_path = string("/boot/System.map-") + kernel_release;
    }
  // Warn in case the target kernel release doesn't match the running one.
  if (last_pass > 4 &&
      (release != kernel_release ||
       machine != architecture)) // NB: squashed ARCH by PR4186 logic
   {
     if(! suppress_warnings)
       cerr << "WARNING: kernel release/architecture mismatch with host forces last-pass 4." << endl;
     last_pass = 4;
   }

  for (int i = optind; i < argc; i++)
    {
      if (! have_script)
        {
          script_file = string (argv[i]);
          have_script = true;
        }
      else
        args.push_back (string (argv[i]));
    }

  // need a user file
  // NB: this is also triggered if stap is invoked with no arguments at all
  if (! have_script)
    {
      // We don't need a script if --list-servers was specified
      if (server_status_strings.empty ())
	{
	  cerr << "A script must be specified." << endl;
	  usage(1);
	}
    }

  // translate path of runtime to absolute path
  if (runtime_path[0] != '/')
    {
      char cwd[PATH_MAX];
      if (getcwd(cwd, sizeof(cwd)))
        {
          runtime_path = string(cwd) + "/" + runtime_path;
        }
    }
}

int
systemtap_session::parse_kernel_config ()
{
  // PR10702: pull config options
  string kernel_config_file = kernel_build_tree + "/.config";
  struct stat st;
  int rc = stat(kernel_config_file.c_str(), &st);
  if (rc != 0)
    {
	clog << "Checking \"" << kernel_config_file << "\" failed: " << strerror(errno) << endl
	     << "Ensure kernel development headers & makefiles are installed." << endl;
	return rc;
    }

  ifstream kcf (kernel_config_file.c_str());
  string line;
  while (getline (kcf, line))
    {
      if (!startswith(line, "CONFIG_")) continue;
      size_t off = line.find('=');
      if (off == string::npos) continue;
      string key = line.substr(0, off);
      string value = line.substr(off+1, string::npos);
      kernel_config[key] = value;
    }
  if (verbose > 2)
    clog << "Parsed kernel \"" << kernel_config_file << "\", number of tuples: " << kernel_config.size() << endl;
  
  kcf.close();
  return 0;
}

int
systemtap_session::parse_kernel_exports ()
{
  string kernel_exports_file = kernel_build_tree + "/Module.symvers";
  struct stat st;
  int rc = stat(kernel_exports_file.c_str(), &st);
  if (rc != 0)
    {
	clog << "Checking \"" << kernel_exports_file << "\" failed: " << strerror(errno) << endl
	     << "Ensure kernel development headers & makefiles are installed." << endl;
	return rc;
    }

  ifstream kef (kernel_exports_file.c_str());
  string line;
  while (getline (kef, line))
    {
      vector<string> tokens;
      tokenize (line, tokens, "\t");
      if (tokens.size() == 4 &&
          tokens[2] == "vmlinux" &&
          tokens[3].substr(0,13) == string("EXPORT_SYMBOL"))
        kernel_exports.insert (tokens[1]);
    }
  if (verbose > 2)
    clog << "Parsed kernel \"" << kernel_exports_file << "\", number of vmlinux exports: " << kernel_exports.size() << endl;
  
  kef.close();
  return 0;
}

static void
uniq_list(list<string>& l)
{
	list<string> r;
	set<string> s;

	for (list<string>::iterator i = l.begin(); i != l.end(); ++i) {
		s.insert(*i);
	}

	for (list<string>::iterator i = l.begin(); i != l.end(); ++i) {
		if (s.find(*i) != s.end()) {
			s.erase(*i);
			r.push_back(*i);
		}
	}

	l.clear();
	l.assign(r.begin(), r.end());
}

void
systemtap_session::printscript(ostream& o)
{
  if (listing_mode)
    {
      // We go through some heroic measures to produce clean output.
      // Record the alias and probe pointer as <name, set<derived_probe *> >
      map<string,set<derived_probe *> > probe_list;

      // Pre-process the probe alias
      for (unsigned i=0; i<probes.size(); i++)
        {
          if (pending_interrupts) return;

          derived_probe* p = probes[i];
          // NB: p->basest() is not so interesting;
          // p->almost_basest() doesn't quite work, so ...
          vector<probe*> chain;
          p->collect_derivation_chain (chain);
          probe* second = (chain.size()>1) ? chain[chain.size()-2] : chain[0];

          #if 0  // dump everything about the derivation chain
          p->printsig(cerr); cerr << endl;
          cerr << "chain[" << chain.size() << "]:" << endl;
          for (unsigned j=0; j<chain.size(); j++)
            {
              cerr << "  [" << j << "]: " << endl;
              cerr << "\tlocations[" << chain[j]->locations.size() << "]:" << endl;
              for (unsigned k=0; k<chain[j]->locations.size(); k++)
                {
                  cerr << "\t  [" << k << "]: ";
                  chain[j]->locations[k]->print(cerr);
                  cerr << endl;
                }
              const probe_alias *a = chain[j]->get_alias();
              if (a)
                {
                  cerr << "\taliases[" << a->alias_names.size() << "]:" << endl;
                  for (unsigned k=0; k<a->alias_names.size(); k++)
                    {
                      cerr << "\t  [" << k << "]: ";
                      a->alias_names[k]->print(cerr);
                      cerr << endl;
                    }
                }
            }
          #endif

          stringstream tmps;
          const probe_alias *a = second->get_alias();
          if (a)
            {
              assert (a->alias_names.size() >= 1);
              a->alias_names[0]->print(tmps); // XXX: [0] is arbitrary; perhaps print all
            }
          else
            {
              assert (second->locations.size() >= 1);
              second->locations[0]->print(tmps); // XXX: [0] is less arbitrary here, but still ...
            }
          string pp = tmps.str();

          // Now duplicate-eliminate.  An alias may have expanded to
          // several actual derived probe points, but we only want to
          // print the alias head name once.
          probe_list[pp].insert(p);
        }

      // print probe name and variables if there
      for (map<string, set<derived_probe *> >::iterator it=probe_list.begin(); it!=probe_list.end(); ++it)
        {
          o << it->first; // probe name or alias

          // Print the locals and arguments for -L mode only
          if (listing_mode_vars)
            {
              map<string,unsigned> var_count; // format <"name:type",count>
              map<string,unsigned> arg_count;
              list<string> var_list;
              list<string> arg_list;
              // traverse set<derived_probe *> to collect all locals and arguments
              for (set<derived_probe *>::iterator ix=it->second.begin(); ix!=it->second.end(); ++ix)
                {
                  derived_probe* p = *ix;
                  // collect available locals of the probe
                  for (unsigned j=0; j<p->locals.size(); j++)
                    {
                      stringstream tmps;
                      vardecl* v = p->locals[j];
                      v->printsig (tmps);
                      var_count[tmps.str()]++;
		      var_list.push_back(tmps.str());
                    }
                  // collect arguments of the probe if there
                  list<string> arg_set;
                  p->getargs(arg_set);
                  for (list<string>::iterator ia=arg_set.begin(); ia!=arg_set.end(); ++ia) {
                    arg_count[*ia]++;
                    arg_list.push_back(*ia);
		  }
                }

	      uniq_list(arg_list);
	      uniq_list(var_list);

              // print the set-intersection only
              for (list<string>::iterator ir=var_list.begin(); ir!=var_list.end(); ++ir)
                if (var_count.find(*ir)->second == it->second.size()) // print locals
                  o << " " << *ir;
              for (list<string>::iterator ir=arg_list.begin(); ir!=arg_list.end(); ++ir)
                if (arg_count.find(*ir)->second == it->second.size()) // print arguments
                  o << " " << *ir;
            }
          o << endl;
        }
    }
  else
    {
      if (embeds.size() > 0)
        o << "# global embedded code" << endl;
      for (unsigned i=0; i<embeds.size(); i++)
        {
          if (pending_interrupts) return;
          embeddedcode* ec = embeds[i];
          ec->print (o);
          o << endl;
        }

      if (globals.size() > 0)
        o << "# globals" << endl;
      for (unsigned i=0; i<globals.size(); i++)
        {
          if (pending_interrupts) return;
          vardecl* v = globals[i];
          v->printsig (o);
          if (verbose && v->init)
            {
              o << " = ";
              v->init->print(o);
            }
          o << endl;
        }

      if (functions.size() > 0)
        o << "# functions" << endl;
      for (map<string,functiondecl*>::iterator it = functions.begin(); it != functions.end(); it++)
        {
          if (pending_interrupts) return;
          functiondecl* f = it->second;
          f->printsig (o);
          o << endl;
          if (f->locals.size() > 0)
            o << "  # locals" << endl;
          for (unsigned j=0; j<f->locals.size(); j++)
            {
              vardecl* v = f->locals[j];
              o << "  ";
              v->printsig (o);
              o << endl;
            }
          if (verbose)
            {
              f->body->print (o);
              o << endl;
            }
        }

      if (probes.size() > 0)
        o << "# probes" << endl;
      for (unsigned i=0; i<probes.size(); i++)
        {
          if (pending_interrupts) return;
          derived_probe* p = probes[i];
          p->printsig (o);
          o << endl;
          if (p->locals.size() > 0)
            o << "  # locals" << endl;
          for (unsigned j=0; j<p->locals.size(); j++)
            {
              vardecl* v = p->locals[j];
              o << "  ";
              v->printsig (o);
              o << endl;
            }
          if (verbose)
            {
              p->body->print (o);
              o << endl;
            }
        }
    }
}


void systemtap_session::insert_loaded_modules()
{
  char line[1024];
  ifstream procmods ("/proc/modules");
  while (procmods.good()) {
    procmods.getline (line, sizeof(line));
    strtok(line, " \t");
    if (line[0] == '\0')
      break;  // maybe print a warning?
    unwindsym_modules.insert (string (line));
  }
  procmods.close();
  unwindsym_modules.insert ("kernel");
}

void
systemtap_session::setup_kernel_release (const char* kstr) 
{
  if (kstr[0] == '/') // fully specified path
    {
      kernel_build_tree = kstr;
      string version_file_name = kernel_build_tree + "/include/config/kernel.release";
      // The file include/config/kernel.release within the
      // build tree is used to pull out the version information
      ifstream version_file (version_file_name.c_str());
      if (version_file.fail ())
	{
	  cerr << "Missing " << version_file_name << endl;
	  exit(1);
	}
      else
	{
	  char c;
	  kernel_release = "";
	  while (version_file.get(c) && c != '\n')
	    kernel_release.push_back(c);
	}
    }
  else
    {
      kernel_release = string (kstr);
      kernel_build_tree = "/lib/modules/" + kernel_release + "/build";
    }
}


// Register all the aliases we've seen in library files, and the user
// file, as patterns.
void
systemtap_session::register_library_aliases()
{
  vector<stapfile*> files(library_files);
  files.push_back(user_file);

  for (unsigned f = 0; f < files.size(); ++f)
    {
      stapfile * file = files[f];
      for (unsigned a = 0; a < file->aliases.size(); ++a)
	{
	  probe_alias * alias = file->aliases[a];
          try
            {
              for (unsigned n = 0; n < alias->alias_names.size(); ++n)
                {
                  probe_point * name = alias->alias_names[n];
                  match_node * n = pattern_root;
                  for (unsigned c = 0; c < name->components.size(); ++c)
                    {
                      probe_point::component * comp = name->components[c];
                      // XXX: alias parameters
                      if (comp->arg)
                        throw semantic_error("alias component "
                                             + comp->functor
                                             + " contains illegal parameter");
                      n = n->bind(comp->functor);
                    }
                  n->bind(new alias_expansion_builder(alias));
                }
            }
          catch (const semantic_error& e)
            {
              semantic_error* er = new semantic_error (e); // copy it
              stringstream msg;
              msg << e.msg2;
              msg << " while registering probe alias ";
              alias->printsig(msg);
              er->msg2 = msg.str();
              print_error (* er);
              delete er;
            }
	}
    }
}


// Print this given token, but abbreviate it if the last one had the
// same file name.
void
systemtap_session::print_token (ostream& o, const token* tok)
{
  assert (tok);

  if (last_token && last_token->location.file == tok->location.file)
    {
      stringstream tmpo;
      tmpo << *tok;
      string ts = tmpo.str();
      // search & replace the file name with nothing
      size_t idx = ts.find (tok->location.file->name);
      if (idx != string::npos)
          ts.replace (idx, tok->location.file->name.size(), "");

      o << ts;
    }
  else
    o << *tok;

  last_token = tok;
}



void
systemtap_session::print_error (const semantic_error& e)
{
  string message_str[2];
  string align_semantic_error ("        ");

  // We generate two messages.  The second one ([1]) is printed
  // without token compression, for purposes of duplicate elimination.
  // This way, the same message that may be generated once with a
  // compressed and once with an uncompressed token still only gets
  // printed once.
  for (int i=0; i<2; i++)
    {
      stringstream message;

      message << "semantic error: " << e.what ();
      if (e.tok1 || e.tok2)
        message << ": ";
      if (e.tok1)
        {
          if (i == 0) print_token (message, e.tok1);
          else message << *e.tok1;
        }
      message << e.msg2;
      if (e.tok2)
        {
          if (i == 0) print_token (message, e.tok2);
          else message << *e.tok2;
        }
      message << endl;
      message_str[i] = message.str();
    }

  // Duplicate elimination
  if (seen_errors.find (message_str[1]) == seen_errors.end())
    {
      seen_errors.insert (message_str[1]);
      cerr << message_str[0];

      if (e.tok1)
        print_error_source (cerr, align_semantic_error, e.tok1);

      if (e.tok2)
        print_error_source (cerr, align_semantic_error, e.tok2);
    }

  if (e.chain)
    print_error (* e.chain);
}

void
systemtap_session::print_error_source (std::ostream& message,
                                       std::string& align, const token* tok)
{
  unsigned i = 0;

  assert (tok);
  if (!tok->location.file)
    //No source to print, silently exit
    return;

  unsigned line = tok->location.line;
  unsigned col = tok->location.column;
  const string &file_contents = tok->location.file->file_contents;

  size_t start_pos = 0, end_pos = 0;
  //Navigate to the appropriate line
  while (i != line && end_pos != std::string::npos)
    {
      start_pos = end_pos;
      end_pos = file_contents.find ('\n', start_pos) + 1;
      i++;
    }
  message << align << "source: " << file_contents.substr (start_pos, end_pos-start_pos-1) << endl;
  message << align << "        ";
  //Navigate to the appropriate column
  for (i=start_pos; i<start_pos+col-1; i++)
    {
      if(isspace(file_contents[i]))
	message << file_contents[i];
      else
	message << ' ';
    }
  message << "^" << endl;
}

void
systemtap_session::print_warning (const string& message_str, const token* tok)
{
  // Duplicate elimination
  string align_warning (" ");
  if (seen_warnings.find (message_str) == seen_warnings.end())
    {
      seen_warnings.insert (message_str);
      clog << "WARNING: " << message_str;
      if (tok) { clog << ": "; print_token (clog, tok); }
      clog << endl;
      if (tok) { print_error_source (clog, align_warning, tok); }
    }
}

string &
systemtap_session::get_host_name ()
{
  if (host_name.empty ())
    get_host_and_domain_name ();
  return host_name;
}

string &
systemtap_session::get_domain_name ()
{
  if (domain_name.empty ())
    get_host_and_domain_name ();
  return domain_name;
}

void
systemtap_session::get_host_and_domain_name ()
{
  // We can't rely on the existence of MAXHOSTNAMELEN, so loop until we get
  // a buffer big enough.
  size_t bufSize = 0;
  char *buf;
  for (;;)
    {
      bufSize += 100;
      buf = new char[bufSize];
      int rc = gethostname (buf, bufSize);
      if (rc == 0)
	break;
      assert (errno == ENAMETOOLONG);
      delete[] buf;
    }
  host_name = buf;
  delete[] buf;

  // Break the returned name up into host and domain.
  string::size_type dot_index = host_name.find ('.');
  if (dot_index != string::npos)
    {
      domain_name = host_name.substr (dot_index + 1);
      host_name = host_name.substr (0, dot_index);
    }
  else
    domain_name = "unknown";
}

// --------------------------------------------------------------------------

/*
Perngrq sebz fzvyrlgnc.fit, rkcbegrq gb n 1484k1110 fzvyrlgnc.cat,
gurapr  catgbcnz | cazfpnyr -jvqgu 160 | 
cczqvgure -qvz 4 -erq 2 -terra 2 -oyhr 2  | cczgbnafv -2k4 | bq -i -j19 -g k1 | 
phg -s2- -q' ' | frq -r 'f,^,\\k,' -r 'f, ,\\k,t' -r 'f,^,",'  -r 'f,$,",'
*/
const char*
systemtap_session::morehelp =
"\x1b\x5b\x30\x6d\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x60\x20\x20\x2e\x60\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x1b\x5b"
"\x33\x33\x6d\x20\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x1b\x5b\x33\x33\x6d\x20\x60"
"\x2e\x60\x1b\x5b\x33\x37\x6d\x20\x3a\x2c\x3a\x2e\x60\x20\x60\x20\x60\x20\x60"
"\x2c\x3b\x2c\x3a\x20\x1b\x5b\x33\x33\x6d\x60\x2e\x60\x20\x1b\x5b\x33\x37\x6d"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x1b\x5b\x33"
"\x33\x6d\x20\x60\x20\x60\x20\x3a\x27\x60\x1b\x5b\x33\x37\x6d\x20\x60\x60\x60"
"\x20\x20\x20\x60\x20\x60\x60\x60\x20\x1b\x5b\x33\x33\x6d\x60\x3a\x60\x20\x60"
"\x20\x60\x20\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x2e\x1b\x5b\x33\x33\x6d\x60\x2e\x60\x20\x60\x20\x60\x20\x20\x1b\x5b\x33"
"\x37\x6d\x20\x3a\x20\x20\x20\x60\x20\x20\x20\x60\x20\x20\x2e\x1b\x5b\x33\x33"
"\x6d\x60\x20\x60\x2e\x60\x20\x60\x2e\x60\x20\x1b\x5b\x33\x37\x6d\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x2e\x3a\x20\x20"
"\x20\x20\x20\x20\x20\x20\x2e\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x2e\x76\x53\x1b\x5b\x33\x34\x6d\x53\x1b\x5b\x33\x37\x6d\x53\x1b\x5b"
"\x33\x31\x6d\x2b\x1b\x5b\x33\x33\x6d\x60\x20\x60\x20\x60\x20\x20\x20\x20\x1b"
"\x5b\x33\x31\x6d\x3f\x1b\x5b\x33\x30\x6d\x53\x1b\x5b\x33\x33\x6d\x2b\x1b\x5b"
"\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x2e\x1b\x5b\x33\x30\x6d\x24\x1b\x5b"
"\x33\x37\x6d\x3b\x1b\x5b\x33\x31\x6d\x7c\x1b\x5b\x33\x33\x6d\x20\x60\x20\x60"
"\x20\x60\x20\x60\x1b\x5b\x33\x31\x6d\x2c\x1b\x5b\x33\x32\x6d\x53\x1b\x5b\x33"
"\x37\x6d\x53\x53\x3e\x2c\x2e\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x2e"
"\x3b\x27\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x3c\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x2e\x2e\x3a\x1b\x5b\x33\x30\x6d\x26\x46\x46\x46\x48\x46\x1b\x5b"
"\x33\x33\x6d\x60\x2e\x60\x20\x60\x20\x60\x20\x60\x1b\x5b\x33\x30\x6d\x4d\x4d"
"\x46\x1b\x5b\x33\x33\x6d\x20\x20\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x1b\x5b"
"\x33\x33\x6d\x20\x3a\x1b\x5b\x33\x30\x6d\x4d\x4d\x46\x1b\x5b\x33\x33\x6d\x20"
"\x20\x20\x60\x20\x60\x2e\x60\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x30\x6d\x46"
"\x46\x46\x24\x53\x46\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x20\x0a\x20\x20\x20"
"\x20\x2e\x3c\x3a\x60\x20\x20\x20\x20\x2e\x3a\x2e\x3a\x2e\x2e\x3b\x27\x20\x20"
"\x20\x20\x20\x20\x2e\x60\x2e\x3a\x60\x60\x3c\x27\x1b\x5b\x33\x31\x6d\x3c\x27"
"\x1b\x5b\x33\x33\x6d\x20\x60\x20\x60\x20\x60\x20\x20\x20\x60\x3c\x1b\x5b\x33"
"\x30\x6d\x26\x1b\x5b\x33\x31\x6d\x3f\x1b\x5b\x33\x33\x6d\x20\x1b\x5b\x33\x37"
"\x6d\x20\x1b\x5b\x33\x33\x6d\x20\x20\x20\x20\x20\x1b\x5b\x33\x37\x6d\x60\x1b"
"\x5b\x33\x30\x6d\x2a\x46\x1b\x5b\x33\x37\x6d\x27\x1b\x5b\x33\x33\x6d\x20\x60"
"\x20\x60\x20\x60\x20\x60\x20\x1b\x5b\x33\x31\x6d\x60\x3a\x1b\x5b\x33\x37\x6d"
"\x27\x3c\x1b\x5b\x33\x30\x6d\x23\x1b\x5b\x33\x37\x6d\x3c\x60\x3a\x20\x20\x20"
"\x0a\x20\x20\x20\x20\x3a\x60\x3a\x60\x20\x20\x20\x60\x3a\x2e\x2e\x2e\x2e\x3c"
"\x3c\x20\x20\x20\x20\x20\x20\x3a\x2e\x60\x3a\x60\x20\x20\x20\x60\x1b\x5b\x33"
"\x33\x6d\x3a\x1b\x5b\x33\x31\x6d\x60\x1b\x5b\x33\x33\x6d\x20\x60\x2e\x60\x20"
"\x60\x20\x60\x20\x60\x20\x60\x1b\x5b\x33\x37\x6d\x20\x20\x1b\x5b\x33\x33\x6d"
"\x20\x60\x20\x20\x20\x60\x1b\x5b\x33\x37\x6d\x20\x60\x20\x60\x1b\x5b\x33\x33"
"\x6d\x20\x60\x2e\x60\x20\x60\x2e\x60\x20\x60\x3a\x1b\x5b\x33\x37\x6d\x20\x20"
"\x20\x60\x3a\x2e\x60\x2e\x20\x0a\x20\x20\x20\x60\x3a\x60\x3a\x60\x20\x20\x20"
"\x20\x20\x60\x60\x60\x60\x20\x3a\x2d\x20\x20\x20\x20\x20\x60\x20\x60\x20\x20"
"\x20\x20\x20\x60\x1b\x5b\x33\x33\x6d\x3a\x60\x2e\x60\x20\x60\x20\x60\x20\x60"
"\x20\x60\x20\x20\x2e\x3b\x1b\x5b\x33\x31\x6d\x76\x1b\x5b\x33\x30\x6d\x24\x24"
"\x24\x1b\x5b\x33\x31\x6d\x2b\x53\x1b\x5b\x33\x33\x6d\x2c\x60\x20\x60\x20\x60"
"\x20\x60\x20\x60\x20\x60\x2e\x1b\x5b\x33\x31\x6d\x60\x1b\x5b\x33\x33\x6d\x3a"
"\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x60\x2e\x60\x20\x20\x0a\x20\x20\x20\x60"
"\x3a\x3a\x3a\x3a\x20\x20\x20\x20\x3a\x60\x60\x60\x60\x3a\x53\x20\x20\x20\x20"
"\x20\x20\x3a\x2e\x60\x2e\x20\x20\x20\x20\x20\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b"
"\x33\x31\x6d\x3a\x1b\x5b\x33\x33\x6d\x2e\x60\x2e\x60\x20\x60\x2e\x60\x20\x60"
"\x20\x3a\x1b\x5b\x33\x30\x6d\x24\x46\x46\x48\x46\x46\x46\x46\x46\x1b\x5b\x33"
"\x31\x6d\x53\x1b\x5b\x33\x33\x6d\x2e\x60\x20\x60\x2e\x60\x20\x60\x2e\x60\x2e"
"\x1b\x5b\x33\x31\x6d\x3a\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b\x33\x37\x6d\x20\x20"
"\x20\x2e\x60\x2e\x3a\x20\x20\x0a\x20\x20\x20\x60\x3a\x3a\x3a\x60\x20\x20\x20"
"\x60\x3a\x20\x2e\x20\x3b\x27\x3a\x20\x20\x20\x20\x20\x20\x3a\x2e\x60\x3a\x20"
"\x20\x20\x20\x20\x3a\x1b\x5b\x33\x33\x6d\x3c\x3a\x1b\x5b\x33\x31\x6d\x60\x1b"
"\x5b\x33\x33\x6d\x2e\x60\x20\x60\x20\x60\x20\x60\x2e\x1b\x5b\x33\x30\x6d\x53"
"\x46\x46\x46\x53\x46\x46\x46\x53\x46\x46\x1b\x5b\x33\x33\x6d\x20\x60\x20\x60"
"\x20\x60\x2e\x60\x2e\x60\x3a\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x37\x6d\x20"
"\x20\x20\x20\x3a\x60\x3a\x60\x20\x20\x0a\x20\x20\x20\x20\x60\x3c\x3b\x3c\x20"
"\x20\x20\x20\x20\x60\x60\x60\x20\x3a\x3a\x20\x20\x20\x20\x20\x20\x20\x3a\x3a"
"\x2e\x60\x20\x20\x20\x20\x20\x3a\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d"
"\x3c\x3a\x60\x1b\x5b\x33\x33\x6d\x2e\x60\x2e\x60\x20\x60\x3a\x1b\x5b\x33\x30"
"\x6d\x53\x46\x53\x46\x46\x46\x53\x46\x46\x46\x53\x1b\x5b\x33\x33\x6d\x2e\x60"
"\x20\x60\x2e\x60\x2e\x60\x3a\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x33\x6d\x3b"
"\x1b\x5b\x33\x37\x6d\x27\x20\x20\x20\x60\x3a\x3a\x60\x20\x20\x20\x0a\x20\x20"
"\x20\x20\x20\x60\x3b\x3c\x20\x20\x20\x20\x20\x20\x20\x3a\x3b\x60\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x60\x3a\x60\x2e\x20\x20\x20\x20\x20\x3a\x1b\x5b\x33"
"\x33\x6d\x3c\x3b\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b\x33"
"\x31\x6d\x3a\x1b\x5b\x33\x33\x6d\x2e\x60\x2e\x60\x20\x1b\x5b\x33\x31\x6d\x3a"
"\x1b\x5b\x33\x30\x6d\x46\x53\x46\x53\x46\x53\x46\x53\x46\x1b\x5b\x33\x31\x6d"
"\x3f\x1b\x5b\x33\x33\x6d\x20\x60\x2e\x60\x2e\x3a\x3a\x1b\x5b\x33\x31\x6d\x3c"
"\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x37\x6d\x60\x20"
"\x20\x20\x3a\x3a\x3a\x60\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x53\x3c"
"\x20\x20\x20\x20\x20\x20\x3a\x53\x3a\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x60\x3a\x3a\x60\x2e\x20\x20\x20\x20\x60\x3a\x1b\x5b\x33\x31\x6d\x3c\x1b"
"\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x3c\x3b\x3c\x1b\x5b\x33\x33\x6d\x3a"
"\x60\x2e\x60\x3c\x1b\x5b\x33\x30\x6d\x53\x46\x53\x24\x53\x46\x53\x24\x1b\x5b"
"\x33\x33\x6d\x60\x3a\x1b\x5b\x33\x31\x6d\x3a\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b"
"\x33\x31\x6d\x3a\x3b\x3c\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x3c\x1b"
"\x5b\x33\x33\x6d\x3a\x1b\x5b\x33\x37\x6d\x60\x20\x20\x2e\x60\x3a\x3a\x60\x20"
"\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x3b\x3c\x2e\x2e\x2c\x2e\x2e\x20"
"\x3a\x3c\x3b\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x3a\x3a\x3a"
"\x60\x20\x20\x20\x20\x20\x60\x3a\x1b\x5b\x33\x33\x6d\x3c\x3b\x1b\x5b\x33\x31"
"\x6d\x3c\x3b\x3c\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33"
"\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x3c\x3c\x1b\x5b\x33\x30\x6d\x53\x24\x53\x1b"
"\x5b\x33\x31\x6d\x53\x1b\x5b\x33\x37\x6d\x27\x1b\x5b\x33\x33\x6d\x2e\x3a\x3b"
"\x1b\x5b\x33\x31\x6d\x3c\x3b\x3c\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b\x33\x31\x6d"
"\x3c\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b\x33\x37\x6d\x60\x20\x20\x20\x60\x2e\x3a"
"\x3a\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x2e\x3a\x3a\x3c\x53\x3c\x3a\x60"
"\x3a\x3a\x3a\x3a\x53\x1b\x5b\x33\x32\x6d\x53\x1b\x5b\x33\x37\x6d\x3b\x27\x3a"
"\x3c\x2c\x2e\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x3a\x3a\x3a\x3a\x2e\x60"
"\x2e\x60\x2e\x60\x3a\x1b\x5b\x33\x33\x6d\x3c\x3a\x1b\x5b\x33\x31\x6d\x3c\x1b"
"\x5b\x33\x33\x6d\x53\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b"
"\x33\x31\x6d\x3c\x2c\x1b\x5b\x33\x33\x6d\x3c\x3b\x3a\x1b\x5b\x33\x31\x6d\x2c"
"\x1b\x5b\x33\x33\x6d\x3c\x3b\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x33\x6d\x53"
"\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x33\x6d\x3b\x3c\x1b\x5b\x33\x37\x6d\x3a"
"\x60\x2e\x60\x2e\x3b\x1b\x5b\x33\x34\x6d\x53\x1b\x5b\x33\x37\x6d\x53\x3f\x27"
"\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x2e\x60\x3a\x60\x3a\x3c\x53\x53\x3b\x3c"
"\x3a\x60\x3a\x3a\x53\x53\x53\x3c\x3a\x60\x3a\x1b\x5b\x33\x30\x6d\x53\x1b\x5b"
"\x33\x37\x6d\x2b\x20\x20\x20\x20\x20\x20\x60\x20\x20\x20\x3a\x1b\x5b\x33\x34"
"\x6d\x53\x1b\x5b\x33\x30\x6d\x53\x46\x24\x1b\x5b\x33\x37\x6d\x2c\x60\x3a\x3a"
"\x3a\x3c\x3a\x3c\x1b\x5b\x33\x33\x6d\x53\x1b\x5b\x33\x37\x6d\x3c\x1b\x5b\x33"
"\x33\x6d\x53\x1b\x5b\x33\x31\x6d\x53\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31"
"\x6d\x53\x3b\x53\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x53\x1b\x5b\x33"
"\x33\x6d\x53\x1b\x5b\x33\x37\x6d\x3c\x1b\x5b\x33\x33\x6d\x53\x1b\x5b\x33\x37"
"\x6d\x3c\x53\x3c\x3a\x3a\x3a\x3a\x3f\x1b\x5b\x33\x30\x6d\x53\x24\x48\x1b\x5b"
"\x33\x37\x6d\x27\x60\x20\x60\x20\x20\x20\x20\x20\x20\x0a\x2e\x60\x3a\x60\x2e"
"\x60\x3a\x60\x2e\x60\x3a\x60\x2e\x60\x3a\x60\x2e\x60\x3a\x60\x2e\x1b\x5b\x33"
"\x30\x6d\x53\x46\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x60\x20\x20\x20\x60\x20"
"\x60\x3a\x1b\x5b\x33\x30\x6d\x3c\x46\x46\x46\x1b\x5b\x33\x37\x6d\x3f\x2e\x60"
"\x3a\x60\x3a\x60\x3a\x60\x3a\x60\x3a\x3c\x3a\x60\x3a\x27\x3a\x60\x3a\x60\x3a"
"\x60\x3a\x60\x3b\x1b\x5b\x33\x30\x6d\x53\x46\x48\x46\x1b\x5b\x33\x37\x6d\x27"
"\x20\x60\x20\x60\x20\x60\x20\x20\x20\x20\x0a\x20\x3c\x3b\x3a\x2e\x60\x20\x60"
"\x2e\x60\x20\x60\x2e\x60\x20\x60\x2e\x60\x2c\x53\x1b\x5b\x33\x32\x6d\x53\x1b"
"\x5b\x33\x30\x6d\x53\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x60\x20\x60\x3c\x1b\x5b\x33\x30\x6d\x46\x46\x46\x1b\x5b\x33\x34\x6d"
"\x2b\x1b\x5b\x33\x37\x6d\x3a\x20\x60\x20\x60\x20\x60\x2e\x60\x20\x60\x2e\x60"
"\x20\x60\x2e\x60\x20\x60\x20\x60\x2c\x1b\x5b\x33\x30\x6d\x24\x46\x48\x46\x1b"
"\x5b\x33\x37\x6d\x27\x20\x60\x20\x20\x20\x60\x20\x20\x20\x20\x20\x20\x0a\x20"
"\x60\x3a\x1b\x5b\x33\x30\x6d\x53\x24\x1b\x5b\x33\x37\x6d\x53\x53\x53\x3b\x3c"
"\x2c\x60\x2c\x3b\x3b\x53\x3f\x53\x1b\x5b\x33\x30\x6d\x24\x46\x3c\x1b\x5b\x33"
"\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x20\x60"
"\x3c\x1b\x5b\x33\x30\x6d\x48\x46\x46\x46\x1b\x5b\x33\x37\x6d\x3f\x2e\x60\x20"
"\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x3b\x76\x1b\x5b\x33\x30\x6d"
"\x48\x46\x48\x46\x1b\x5b\x33\x37\x6d\x27\x20\x60\x20\x20\x20\x60\x20\x20\x20"
"\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x60\x3c\x1b\x5b\x33\x30\x6d\x46\x24\x1b"
"\x5b\x33\x37\x6d\x53\x53\x53\x53\x53\x53\x1b\x5b\x33\x30\x6d\x53\x24\x53\x46"
"\x46\x46\x1b\x5b\x33\x37\x6d\x27\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x60\x3c\x1b\x5b\x33\x30\x6d\x23\x46\x46\x46"
"\x24\x1b\x5b\x33\x37\x6d\x76\x2c\x2c\x20\x2e\x20\x2e\x20\x2c\x2c\x76\x1b\x5b"
"\x33\x30\x6d\x26\x24\x46\x46\x48\x3c\x1b\x5b\x33\x37\x6d\x27\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x60"
"\x3c\x1b\x5b\x33\x30\x6d\x53\x46\x46\x24\x46\x24\x46\x46\x48\x46\x53\x1b\x5b"
"\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x2e\x60\x20\x60\x2e\x60\x2e\x60"
"\x2e\x60\x2e\x60\x3a\x3a\x3a\x3a\x3a\x1b\x5b\x33\x30\x6d\x2a\x46\x46\x46\x48"
"\x46\x48\x46\x48\x46\x46\x46\x48\x46\x48\x46\x48\x1b\x5b\x33\x37\x6d\x3c\x22"
"\x2e\x60\x2e\x60\x2e\x60\x2e\x60\x2e\x60\x20\x20\x20\x20\x20\x20\x20\x20\x0a"
"\x20\x20\x20\x20\x20\x20\x20\x60\x3a\x1b\x5b\x33\x30\x6d\x48\x46\x46\x46\x48"
"\x46\x46\x46\x1b\x5b\x33\x37\x6d\x27\x20\x20\x20\x60\x20\x60\x2e\x60\x20\x60"
"\x2e\x60\x2e\x60\x3a\x60\x3a\x60\x3a\x60\x3a\x60\x3a\x3a\x3a\x60\x3a\x3c\x3c"
"\x1b\x5b\x33\x30\x6d\x3c\x46\x48\x46\x46\x46\x48\x46\x46\x46\x1b\x5b\x33\x37"
"\x6d\x27\x3a\x60\x3a\x60\x3a\x60\x3a\x60\x2e\x60\x2e\x60\x20\x60\x2e\x60\x20"
"\x60\x20\x60\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x22\x1b\x5b"
"\x33\x30\x6d\x2a\x46\x48\x46\x1b\x5b\x33\x37\x6d\x3f\x20\x20\x20\x60\x20\x60"
"\x2e\x60\x20\x60\x2e\x60\x2e\x60\x3a\x60\x2e\x60\x3a\x60\x3a\x60\x3a\x60\x3a"
"\x60\x3a\x60\x3a\x60\x3a\x60\x3a\x1b\x5b\x33\x30\x6d\x46\x46\x48\x46\x48\x46"
"\x1b\x5b\x33\x37\x6d\x27\x3a\x60\x3a\x60\x3a\x60\x3a\x60\x2e\x60\x3a\x60\x2e"
"\x60\x2e\x60\x20\x60\x2e\x60\x20\x60\x20\x60\x0a\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x60\x3c\x1b\x5b\x33\x30\x6d\x48\x46\x46\x1b\x5b\x33\x37\x6d"
"\x2b\x60\x20\x20\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x2e\x60\x20"
"\x60\x2e\x60\x20\x60\x2e\x60\x20\x60\x3a\x60\x2e\x60\x3b\x1b\x5b\x33\x30\x6d"
"\x48\x46\x46\x46\x1b\x5b\x33\x37\x6d\x27\x2e\x60\x2e\x60\x20\x60\x2e\x60\x20"
"\x60\x2e\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x20\x20\x60\x20\x20\x0a\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x22\x1b\x5b\x33\x30\x6d\x3c"
"\x48\x46\x53\x1b\x5b\x33\x37\x6d\x2b\x3a\x20\x20\x20\x60\x20\x60\x20\x60\x20"
"\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x2c\x1b"
"\x5b\x33\x30\x6d\x24\x46\x48\x46\x1b\x5b\x33\x37\x6d\x3f\x20\x60\x20\x60\x20"
"\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x20\x20\x60"
"\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x60\x22\x3c\x1b\x5b\x33\x30\x6d\x48\x24\x46\x46\x1b\x5b\x33\x37\x6d\x3e\x2c"
"\x2e\x2e\x20\x20\x20\x20\x20\x20\x20\x20\x60\x20\x20\x20\x60\x20\x20\x20\x3b"
"\x2c\x2c\x1b\x5b\x33\x30\x6d\x24\x53\x46\x46\x46\x1b\x5b\x33\x37\x6d\x27\x22"
"\x20\x20\x60\x20\x20\x20\x60\x20\x20\x20\x60\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x22\x1b\x5b\x33\x30\x6d\x2a\x3c\x48"
"\x46\x46\x24\x53\x24\x1b\x5b\x33\x37\x6d\x53\x53\x53\x3e\x3e\x3e\x3e\x3e\x53"
"\x3e\x53\x1b\x5b\x33\x30\x6d\x24\x53\x24\x46\x24\x48\x46\x23\x1b\x5b\x33\x37"
"\x6d\x27\x22\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x60\x60\x22\x3c\x1b\x5b\x33\x30\x6d\x2a\x3c\x3c\x3c\x48\x46\x46\x46\x48\x46"
"\x46\x46\x23\x3c\x1b\x5b\x33\x36\x6d\x3c\x1b\x5b\x33\x37\x6d\x3c\x27\x22\x22"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x1b"
                                                                "\x5b\x30\x6d";
