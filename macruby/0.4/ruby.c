/**********************************************************************

  ruby.c -

  $Author: nobu $
  created at: Tue Aug 10 12:47:31 JST 1993

  Copyright (C) 1993-2007 Yukihiro Matsumoto
  Copyright (C) 2000  Network Applied Communication Laboratory, Inc.
  Copyright (C) 2000  Information-technology Promotion Agency, Japan

**********************************************************************/

#ifdef __CYGWIN__
#include <windows.h>
#include <sys/cygwin.h>
#endif
#ifdef _WIN32_WCE
#include <winsock.h>
#include "ruby/wince.h"
#endif
#include "ruby/ruby.h"
#include "ruby/node.h"
#include "ruby/encoding.h"
#include "eval_intern.h"
#include "dln.h"
#include <stdio.h>
#include <sys/types.h>
#include <ctype.h>

#ifdef __hpux
#include <sys/pstat.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if defined(HAVE_FCNTL_H)
#include <fcntl.h>
#elif defined(HAVE_SYS_FCNTL_H)
#include <sys/fcntl.h>
#endif
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifndef MAXPATHLEN
# define MAXPATHLEN 1024
#endif

#if defined(__MACOS__) && defined(__MWERKS__)
#include <console.h>
#endif

#include "ruby/util.h"

#ifndef HAVE_STDLIB_H
char *getenv();
#endif

/* TODO: move to VM */
VALUE ruby_debug = Qfalse;
VALUE ruby_verbose = Qfalse;
VALUE rb_parser_get_yydebug(VALUE);
VALUE rb_parser_set_yydebug(VALUE, VALUE);

const char *ruby_get_inplace_mode(void);
void ruby_set_inplace_mode(const char *);

#define DISABLE_BIT(bit) (1U << disable_##bit)
enum disable_flag_bits {
    disable_gems,
    disable_rubyopt,
};

#define DUMP_BIT(bit) (1U << dump_##bit)
enum dump_flag_bits {
    dump_insns,
};

struct cmdline_options {
    int sflag, xflag;
    int do_loop, do_print;
    int do_check, do_line;
    int do_split, do_search;
    int usage;
    int version;
    int copyright;
    unsigned int disable;
    int verbose;
    int yydebug;
    unsigned int dump;
    const char *script;
    VALUE script_name;
    VALUE e_script;
    struct {
	struct {
	    VALUE name;
#if WITH_OBJC
	    rb_encoding *enc;
#else
	    int index;
#endif
	} enc;
    } src, ext;
};

struct cmdline_arguments {
    int argc;
    char **argv;
    struct cmdline_options *opt;
};

static NODE *load_file(VALUE, const char *, int, struct cmdline_options *);
static void forbid_setid(const char *);

static struct {
    int argc;
    char **argv;
#if !defined(PSTAT_SETCMD) && !defined(HAVE_SETPROCTITLE)
    int len;
#endif
} origarg;

static void
usage(const char *name)
{
    /* This message really ought to be max 23 lines.
     * Removed -h because the user already knows that option. Others? */

    static const char *const usage_msg[] = {
	"-0[octal]       specify record separator (\\0, if no argument)",
	"-a              autosplit mode with -n or -p (splits $_ into $F)",
	"-c              check syntax only",
	"-Cdirectory     cd to directory, before executing your script",
	"-d              set debugging flags (set $DEBUG to true)",
	"-e 'command'    one line of script. Several -e's allowed. Omit [programfile]",
	"-Eencoding      specifies the character encoding for the program codes",
	"-Fpattern       split() pattern for autosplit (-a)",
	"-i[extension]   edit ARGV files in place (make backup if extension supplied)",
	"-Idirectory     specify $LOAD_PATH directory (may be used more than once)",
	"-l              enable line ending processing",
	"-n              assume 'while gets(); ... end' loop around your script",
	"-p              assume loop like -n but print line also like sed",
	"-rlibrary       require the library, before executing your script",
	"-s              enable some switch parsing for switches after script name",
	"-S              look for the script using PATH environment variable",
	"-T[level]       turn on tainting checks",
	"-v              print version number, then turn on verbose mode",
	"-w              turn warnings on for your script",
	"-W[level]       set warning level; 0=silence, 1=medium, 2=verbose (default)",
	"-x[directory]   strip off text before #!ruby line and perhaps cd to directory",
	"--copyright     print the copyright",
	"--version       print the version",
	NULL
    };
    const char *const *p = usage_msg;

    printf("Usage: %s [switches] [--] [programfile] [arguments]\n", name);
    while (*p)
	printf("  %s\n", *p++);
}

VALUE rb_get_load_path(void);

#ifndef CharNext		/* defined as CharNext[AW] on Windows. */
#define CharNext(p) ((p) + mblen(p, RUBY_MBCHAR_MAXSIZE))
#endif

#if defined DOSISH || defined __CYGWIN__
static inline void
translate_char(char *p, int from, int to)
{
    while (*p) {
	if ((unsigned char)*p == from)
	    *p = to;
	p = CharNext(p);
    }
}
#endif

#if defined _WIN32 || defined __CYGWIN__ || defined __DJGPP__
static VALUE
rubylib_mangled_path(const char *s, unsigned int l)
{
    static char *newp, *oldp;
    static int newl, oldl, notfound;
    char *ptr;
    VALUE ret;

    if (!newp && !notfound) {
	newp = getenv("RUBYLIB_PREFIX");
	if (newp) {
	    oldp = newp = strdup(newp);
	    while (*newp && !ISSPACE(*newp) && *newp != ';') {
		newp = CharNext(newp);	/* Skip digits. */
	    }
	    oldl = newp - oldp;
	    while (*newp && (ISSPACE(*newp) || *newp == ';')) {
		newp = CharNext(newp);	/* Skip whitespace. */
	    }
	    newl = strlen(newp);
	    if (newl == 0 || oldl == 0) {
		rb_fatal("malformed RUBYLIB_PREFIX");
	    }
	    translate_char(newp, '\\', '/');
	}
	else {
	    notfound = 1;
	}
    }
    if (!newp || l < oldl || STRNCASECMP(oldp, s, oldl) != 0) {
	return rb_str_new(s, l);
    }
    ret = rb_str_new(0, l + newl - oldl);
    ptr = RSTRING_BYTEPTR(ret); /* ok */
    memcpy(ptr, newp, newl);
    memcpy(ptr + newl, s + oldl, l - oldl);
    ptr[l + newl - oldl] = 0;
    RSTRING_SYNC(ret);
    return ret;
}

static VALUE
rubylib_mangled_path2(const char *s)
{
    return rubylib_mangled_path(s, strlen(s));
}
#else
#define rubylib_mangled_path rb_str_new
#define rubylib_mangled_path2 rb_str_new2
#endif

static void
push_include(const char *path, VALUE (*filter)(VALUE))
{
    const char sep = PATH_SEP_CHAR;
    const char *p, *s;
    VALUE load_path = GET_VM()->load_path;

    p = path;
    while (*p) {
	while (*p == sep)
	    p++;
	if (!*p) break;
	for (s = p; *s && *s != sep; s = CharNext(s));
	rb_ary_push(load_path, (*filter)(rubylib_mangled_path(p, s - p)));
	p = s;
    }
}

#ifdef __CYGWIN__
static void
push_include_cygwin(const char *path, VALUE (*filter)(VALUE))
{
    const char *p, *s;
    char rubylib[FILENAME_MAX];
    VALUE buf = 0;

    p = path;
    while (*p) {
	unsigned int len;
	while (*p == ';')
	    p++;
	if (!*p) break;
	for (s = p; *s && *s != ';'; s = CharNext(s));
	len = s - p;
	if (*s) {
	    if (!buf) {
		buf = rb_str_new(p, len);
		p = RSTRING_PTR(buf);
	    }
	    else {
		rb_str_resize(buf, len);
		p = strncpy(RSTRING_BYTEPTR(buf), p, len); /* ok */
		RSTRING_SYNC(buf);
	    }
	}
	if (cygwin_conv_to_posix_path(p, rubylib) == 0)
	    p = rubylib;
	push_include(p, filter);
	if (!*s) break;
	p = s + 1;
    }
}

#define push_include push_include_cygwin
#endif

void
ruby_push_include(const char *path, VALUE (*filter)(VALUE))
{
    if (path == 0)
	return;
    push_include(path, filter);
}

static VALUE
identical_path(VALUE path)
{
    return path;
}

void
ruby_incpush(const char *path)
{
    ruby_push_include(path, identical_path);
}

static VALUE
expand_include_path(VALUE path)
{
    const char *p = RSTRING_PTR(path);
    if (!p)
	return path;
    if (*p == '.' && p[1] == '/')
	return path;
    return rb_file_expand_path(path, Qnil);
}

void 
ruby_incpush_expand(const char *path)
{
    ruby_push_include(path, expand_include_path);
}

#if defined DOSISH || defined __CYGWIN__
#define LOAD_RELATIVE 1
#endif

#if defined _WIN32 || defined __CYGWIN__
static HMODULE libruby;

BOOL WINAPI
DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
	libruby = dll;
    return TRUE;
}
#endif

void
ruby_init_loadpath(void)
{
    VALUE load_path;
#if defined LOAD_RELATIVE
    char libpath[MAXPATHLEN + 1];
    char *p;
    int rest;

#if defined _WIN32 || defined __CYGWIN__
    GetModuleFileName(libruby, libpath, sizeof libpath);
#elif defined(DJGPP)
    extern char *__dos_argv0;
    strncpy(libpath, __dos_argv0, sizeof(libpath) - 1);
#elif defined(__human68k__)
    extern char **_argv;
    strncpy(libpath, _argv[0], sizeof(libpath) - 1);
#elif defined(__EMX__)
    _execname(libpath, sizeof(libpath) - 1);
#endif

    libpath[sizeof(libpath) - 1] = '\0';
#if defined DOSISH
    translate_char(libpath, '\\', '/');
#elif defined __CYGWIN__
    {
	char rubylib[FILENAME_MAX];
	cygwin_conv_to_posix_path(libpath, rubylib);
	strncpy(libpath, rubylib, sizeof(libpath));
    }
#endif
    p = strrchr(libpath, '/');
    if (p) {
	*p = 0;
	if (p - libpath > 3 && !STRCASECMP(p - 4, "/bin")) {
	    p -= 4;
	    *p = 0;
	}
    }
    else {
	strcpy(libpath, ".");
	p = libpath + 1;
    }

    rest = sizeof(libpath) - 1 - (p - libpath);

#define RUBY_RELATIVE(path) (strncpy(p, (path), rest), libpath)
#else
#define RUBY_RELATIVE(path) (path)
#endif
#define incpush(path) rb_ary_push(load_path, rubylib_mangled_path2(path))
    load_path = GET_VM()->load_path;

    if (rb_safe_level() == 0) {
	ruby_incpush(getenv("RUBYLIB"));
    }

#ifdef RUBY_SEARCH_PATH
    incpush(RUBY_RELATIVE(RUBY_SEARCH_PATH));
#endif

    incpush(RUBY_RELATIVE(RUBY_SITE_LIB2));
#ifdef RUBY_SITE_THIN_ARCHLIB
    incpush(RUBY_RELATIVE(RUBY_SITE_THIN_ARCHLIB));
#endif
    incpush(RUBY_RELATIVE(RUBY_SITE_ARCHLIB));
    incpush(RUBY_RELATIVE(RUBY_SITE_LIB));

    incpush(RUBY_RELATIVE(RUBY_VENDOR_LIB2));
#ifdef RUBY_VENDOR_THIN_ARCHLIB
    incpush(RUBY_RELATIVE(RUBY_VENDOR_THIN_ARCHLIB));
#endif
    incpush(RUBY_RELATIVE(RUBY_VENDOR_ARCHLIB));
    incpush(RUBY_RELATIVE(RUBY_VENDOR_LIB));

    incpush(RUBY_RELATIVE(RUBY_LIB));
#ifdef RUBY_THIN_ARCHLIB
    incpush(RUBY_RELATIVE(RUBY_THIN_ARCHLIB));
#endif
    incpush(RUBY_RELATIVE(RUBY_ARCHLIB));

    if (rb_safe_level() == 0) {
	incpush(".");
    }
}

#if WITH_OBJC
static CFMutableArrayRef req_list = NULL;
#else
struct req_list {
    char *name;
    struct req_list *next;
};
static struct {
    struct req_list *last, head;
} req_list = {&req_list.head,};
#endif

static void
add_modules(const char *mod)
{
#if WITH_OBJC
    CFStringRef mod_str;
    if (req_list == NULL) {
	req_list = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    }
    mod_str = CFStringCreateWithFileSystemRepresentation(NULL, mod);
    CFArrayAppendValue(req_list, mod_str);
    CFRelease(mod_str);
#else
    struct req_list *list;

    list = ALLOC(struct req_list);
    list->name = ALLOC_N(char, strlen(mod) + 1);
    strcpy(list->name, mod);
    list->next = 0;
    req_list.last->next = list;
    req_list.last = list;
#endif
}

extern void Init_ext(void);
extern VALUE rb_vm_top_self(void);

static void
require_libraries(void)
{
#if WITH_OBJC
    Init_ext();		/* should be called here for some reason :-( */

    if (req_list != NULL) {
	VALUE vm;
	ID require;
	int i, count;
       
	vm = rb_vm_top_self();
	require	= rb_intern("require");
	for (i = 0, count = CFArrayGetCount(req_list); i < count; i++) {
	    const void *feature = CFArrayGetValueAtIndex(req_list, i);
	    rb_funcall2(vm, require, 1, (VALUE *)&feature);
	}
	CFRelease(req_list);
    }
#else
    struct req_list *list = req_list.head.next;
    struct req_list *tmp;
    ID require = rb_intern("require");

    Init_ext();		/* should be called here for some reason :-( */
    req_list.last = 0;
    while (list) {
	VALUE feature = rb_str_new2(list->name);
	tmp = list->next;
	xfree(list->name);
	xfree(list);
	list = tmp;
	rb_funcall2(rb_vm_top_self(), require, 1, &feature);
    }
    req_list.head.next = 0;
#endif
}

static void
process_sflag(struct cmdline_options *opt)
{
    if (opt->sflag) {
	long i, n;
	VALUE argv = rb_argv;

	n = RARRAY_LEN(argv);
	i = 0;
	while (n > 0) {
	    VALUE v = RARRAY_AT(argv, i++);
	    char *s = StringValuePtr(v);
	    char *p;
	    int hyphen = Qfalse;

	    if (s[0] != '-')
		break;
	    n--;
	    if (s[1] == '-' && s[2] == '\0')
		break;

	    v = Qtrue;
	    /* check if valid name before replacing - with _ */
	    for (p = s + 1; *p; p++) {
		if (*p == '=') {
		    *p++ = '\0';
		    v = rb_str_new2(p);
		    break;
		}
		if (*p == '-') {
		    hyphen = Qtrue;
		}
		else if (*p != '_' && !ISALNUM(*p)) {
		    VALUE name_error[2];
		    name_error[0] =
			rb_str_new2("invalid name for global variable - ");
		    if (!(p = strchr(p, '='))) {
			rb_str_cat2(name_error[0], s);
		    }
		    else {
			rb_str_cat(name_error[0], s, p - s);
		    }
		    name_error[1] = RARRAY_AT(argv, -1);
		    rb_exc_raise(rb_class_new_instance(2, name_error, rb_eNameError));
		}
	    }
	    s[0] = '$';
	    if (hyphen) {
		for (p = s + 1; *p; ++p) {
		    if (*p == '-')
			*p = '_';
		}
	    }
	    rb_gv_set(s, v);
	}
	n = RARRAY_LEN(argv) - n;
	while (n--) {
	    rb_ary_shift(argv);
	}
    }
    opt->sflag = 0;
}

NODE *rb_parser_append_print(VALUE, NODE *);
NODE *rb_parser_while_loop(VALUE, NODE *, int, int);
static int proc_options(int argc, char **argv, struct cmdline_options *opt);

static char *
moreswitches(const char *s, struct cmdline_options *opt)
{
    int argc;
    char *argv[3];
    const char *p = s;

    argc = 2;
    argv[0] = argv[2] = 0;
    while (*s && !ISSPACE(*s))
	s++;
    argv[1] = ALLOCA_N(char, s - p + 2);
    argv[1][0] = '-';
    strncpy(argv[1] + 1, p, s - p);
    argv[1][s - p + 1] = '\0';
    proc_options(argc, argv, opt);
    while (*s && ISSPACE(*s))
	s++;
    return (char *)s;
}

#define NAME_MATCH_P(name, str, len) \
    ((len) < sizeof(name) && strncmp((str), name, (len)) == 0)

#define UNSET_WHEN(name, bit, str, len)	\
    if (NAME_MATCH_P(name, str, len)) { \
	*(unsigned int *)arg &= ~(bit); \
	return;				\
    }

#define SET_WHEN(name, bit, str, len)	\
    if (NAME_MATCH_P(name, str, len)) { \
	*(unsigned int *)arg |= (bit);	\
	return;				\
    }

static void
enable_option(const char *str, int len, void *arg)
{
#define UNSET_WHEN_DISABLE(bit) UNSET_WHEN(#bit, DISABLE_BIT(bit), str, len)
    UNSET_WHEN_DISABLE(gems);
    UNSET_WHEN_DISABLE(rubyopt);
    if (NAME_MATCH_P("all", str, len)) {
	*(unsigned int *)arg = 0U;
	return;
    }
    rb_warn("unknown argument for --enable: `%.*s'", len, str);
}

static void
disable_option(const char *str, int len, void *arg)
{
#define SET_WHEN_DISABLE(bit) SET_WHEN(#bit, DISABLE_BIT(bit), str, len)
    SET_WHEN_DISABLE(gems);
    SET_WHEN_DISABLE(rubyopt);
    if (NAME_MATCH_P("all", str, len)) {
	*(unsigned int *)arg = ~0U;
	return;
    }
    rb_warn("unknown argument for --disable: `%.*s'", len, str);
}

static void
dump_option(const char *str, int len, void *arg)
{
#define SET_WHEN_DUMP(bit) SET_WHEN(#bit, DUMP_BIT(bit), str, len)
    SET_WHEN_DUMP(insns);
    rb_warn("don't know how to dump `%.*s', (insns)", len, str);
}

static int
proc_options(int argc, char **argv, struct cmdline_options *opt)
{
    int n, argc0 = argc;
    const char *s;

    if (argc == 0)
	return 0;

    for (argc--, argv++; argc > 0; argc--, argv++) {
	if (argv[0][0] != '-' || !argv[0][1])
	    break;

	s = argv[0] + 1;
      reswitch:
	switch (*s) {
	  case 'a':
	    opt->do_split = Qtrue;
	    s++;
	    goto reswitch;

	  case 'p':
	    opt->do_print = Qtrue;
	    /* through */
	  case 'n':
	    opt->do_loop = Qtrue;
	    s++;
	    goto reswitch;

	  case 'd':
	    ruby_debug = Qtrue;
	    ruby_verbose = Qtrue;
	    s++;
	    goto reswitch;

	  case 'y':
	    opt->yydebug = 1;
	    s++;
	    goto reswitch;

	  case 'v':
	    if (opt->verbose) {
		s++;
		goto reswitch;
	    }
	    ruby_show_version();
	    opt->verbose = 1;
	  case 'w':
	    ruby_verbose = Qtrue;
	    s++;
	    goto reswitch;

	  case 'W':
	    {
		int numlen;
		int v = 2;	/* -W as -W2 */

		if (*++s) {
		    v = scan_oct(s, 1, &numlen);
		    if (numlen == 0)
			v = 1;
		    s += numlen;
		}
		switch (v) {
		  case 0:
		    ruby_verbose = Qnil;
		    break;
		  case 1:
		    ruby_verbose = Qfalse;
		    break;
		  default:
		    ruby_verbose = Qtrue;
		    break;
		}
	    }
	    goto reswitch;

	  case 'c':
	    opt->do_check = Qtrue;
	    s++;
	    goto reswitch;

	  case 's':
	    forbid_setid("-s");
	    opt->sflag = 1;
	    s++;
	    goto reswitch;

	  case 'h':
	    usage(origarg.argv[0]);
	    rb_exit(EXIT_SUCCESS);
	    break;

	  case 'l':
	    opt->do_line = Qtrue;
	    rb_output_rs = rb_rs;
	    s++;
	    goto reswitch;

	  case 'S':
	    forbid_setid("-S");
	    opt->do_search = Qtrue;
	    s++;
	    goto reswitch;

	  case 'e':
	    forbid_setid("-e");
	    if (!*++s) {
		s = argv[1];
		argc--, argv++;
	    }
	    if (!s) {
		rb_raise(rb_eRuntimeError, "no code specified for -e");
	    }
	    if (!opt->e_script) {
		opt->e_script = rb_str_new(0, 0);
		if (opt->script == 0)
		    opt->script = "-e";
	    }
	    rb_str_cat2(opt->e_script, s);
	    rb_str_cat2(opt->e_script, "\n");
	    break;

	  case 'r':
	    forbid_setid("-r");
	    if (*++s) {
		add_modules(s);
	    }
	    else if (argv[1]) {
		add_modules(argv[1]);
		argc--, argv++;
	    }
	    break;

	  case 'i':
	    forbid_setid("-i");
	    ruby_set_inplace_mode(s + 1);
	    break;

	  case 'x':
	    opt->xflag = Qtrue;
	    s++;
	    if (*s && chdir(s) < 0) {
		rb_fatal("Can't chdir to %s", s);
	    }
	    break;

	  case 'C':
	  case 'X':
	    s++;
	    if (!*s) {
		s = argv[1];
		argc--, argv++;
	    }
	    if (!s || !*s) {
		rb_fatal("Can't chdir");
	    }
	    if (chdir(s) < 0) {
		rb_fatal("Can't chdir to %s", s);
	    }
	    break;

	  case 'F':
	    if (*++s) {
		rb_fs = rb_reg_new(s, strlen(s), 0);
	    }
	    break;

	  case 'E':
	    if (!*++s) goto next_encoding;
	    goto encoding;

	  case 'K':
	    if (*++s) {
		const char *enc_name = 0;
		switch (*s) {
		  case 'E': case 'e':
		    enc_name = "EUC-JP";
		    break;
		  case 'S': case 's':
		    enc_name = "Windows-31J";
		    break;
		  case 'U': case 'u':
		    enc_name = "UTF-8";
		    break;
		  case 'N': case 'n': case 'A': case 'a':
		    enc_name = "ASCII-8BIT";
		    break;
		}
		if (enc_name) {
		    opt->src.enc.name = rb_str_new2(enc_name);
		    opt->ext.enc.name = opt->src.enc.name;
		}
		s++;
	    }
	    goto reswitch;

	  case 'T':
	    {
		int numlen;
		int v = 1;

		if (*++s) {
		    v = scan_oct(s, 2, &numlen);
		    if (numlen == 0)
			v = 1;
		    s += numlen;
		}
		rb_set_safe_level(v);
	    }
	    goto reswitch;

	  case 'I':
	    forbid_setid("-I");
	    if (*++s)
		ruby_incpush_expand(s);
	    else if (argv[1]) {
		ruby_incpush_expand(argv[1]);
		argc--, argv++;
	    }
	    break;

	  case '0':
	    {
		int numlen;
		int v;
		char c;

		v = scan_oct(s, 4, &numlen);
		s += numlen;
		if (v > 0377)
		    rb_rs = Qnil;
		else if (v == 0 && numlen >= 2) {
		    rb_rs = rb_str_new2("\n\n");
		}
		else {
		    c = v & 0xff;
		    rb_rs = rb_str_new(&c, 1);
		}
	    }
	    goto reswitch;

	  case '-':
	    if (!s[1] || (s[1] == '\r' && !s[2])) {
		argc--, argv++;
		goto switch_end;
	    }
	    s++;
	    if (strcmp("copyright", s) == 0)
		opt->copyright = 1;
	    else if (strcmp("debug", s) == 0) {
		ruby_debug = Qtrue;
                ruby_verbose = Qtrue;
            }
	    else if (strncmp("enable", s, n = 6) == 0 &&
		     (!s[n] || s[n] == '-' || s[n] == '=')) {
		if ((s += n + 1)[-1] ? !*s : (!--argc || !(s = *++argv))) {
		    rb_raise(rb_eRuntimeError, "missing argument for --enable");
		}
		ruby_each_words(s, enable_option, &opt->disable);
	    }
	    else if (strncmp("disable", s, n = 7) == 0 &&
		     (!s[n] || s[n] == '-' || s[n] == '=')) {
		if ((s += n + 1)[-1] ? !*s : (!--argc || !(s = *++argv))) {
		    rb_raise(rb_eRuntimeError, "missing argument for --disable");
		}
		ruby_each_words(s, disable_option, &opt->disable);
	    }
	    else if (strncmp("encoding", s, n = 8) == 0 && (!s[n] || s[n] == '=')) {
		s += n;
		if (!*s++) {
		  next_encoding:
		    if (!--argc || !(s = *++argv)) {
			rb_raise(rb_eRuntimeError, "missing argument for --encoding");
		    }
		}
	      encoding:
		opt->ext.enc.name = rb_str_new2(s);
	    }
	    else if (strcmp("version", s) == 0)
		opt->version = 1;
	    else if (strcmp("verbose", s) == 0) {
		opt->verbose = 1;
		ruby_verbose = Qtrue;
	    }
	    else if (strcmp("yydebug", s) == 0)
		opt->yydebug = 1;
	    else if (strncmp("dump", s, n = 4) == 0 && (!s[n] || s[n] == '=')) {
		if (!(s += n + 1)[-1] && (!--argc || !(s = *++argv)) && *s != '-') break;
		ruby_each_words(s, dump_option, &opt->dump);
	    }
	    else if (strcmp("help", s) == 0) {
		usage(origarg.argv[0]);
		rb_exit(EXIT_SUCCESS);
	    }
	    else {
		rb_raise(rb_eRuntimeError,
			 "invalid option --%s  (-h will show valid options)", s);
	    }
	    break;

	  case '\r':
	    if (!s[1])
		break;

	  default:
	    {
		if (ISPRINT(*s)) {
                    rb_raise(rb_eRuntimeError,
			"invalid option -%c  (-h will show valid options)",
                        (int)(unsigned char)*s);
		}
		else {
                    rb_raise(rb_eRuntimeError,
			"invalid option -\\x%02X  (-h will show valid options)",
                        (int)(unsigned char)*s);
		}
	    }
	    goto switch_end;

	  case 0:
	    break;
	}
    }

  switch_end:
    return argc0 - argc;
}

void Init_prelude(void);

static void
ruby_init_gems(int enable)
{
    if (enable) rb_define_module("Gem");
    Init_prelude();
}

#if WITH_OBJC
static rb_encoding *
opt_enc_find(VALUE enc_name)
{
    rb_encoding *enc = rb_enc_find2(enc_name);
    if (enc == NULL)
	rb_raise(rb_eRuntimeError, "unknown encoding name - %s", 
	    RSTRING_PTR(enc_name));
    return enc;
}
#else
static int
opt_enc_index(VALUE enc_name)
{
    const char *s = RSTRING_PTR(enc_name);
    int i = rb_enc_find_index(s);

    if (i < 0) {
	rb_raise(rb_eRuntimeError, "unknown encoding name - %s", s);
    }
    else if (rb_enc_dummy_p(rb_enc_from_index(i))) {
	rb_raise(rb_eRuntimeError, "dummy encoding is not acceptable - %s ", s);
    }
    return i;
}
#endif

VALUE rb_progname;
VALUE rb_argv0;

#if WITH_OBJC
static rb_encoding *src_encoding;
#else
static int src_encoding_index = -1; /* TODO: VM private */
#endif

static VALUE
process_options(VALUE arg)
{
    struct cmdline_arguments *argp = (struct cmdline_arguments *)arg;
    struct cmdline_options *opt = argp->opt;
    int argc = argp->argc;
    char **argv = argp->argv;
    NODE *tree = 0;
    VALUE parser;
    VALUE iseq;
    rb_encoding *enc, *lenc;
    const char *s;
    char fbuf[MAXPATHLEN];
    int i = proc_options(argc, argv, opt);
    int safe;

    argc -= i;
    argv += i;

    if (!(opt->disable & DISABLE_BIT(rubyopt)) &&
	rb_safe_level() == 0 && (s = getenv("RUBYOPT"))) {
	VALUE src_enc_name = opt->src.enc.name;
	VALUE ext_enc_name = opt->ext.enc.name;

	while (ISSPACE(*s))
	    s++;
	if (*s == 'T' || (*s == '-' && *(s + 1) == 'T')) {
	    int numlen;
	    int v = 1;

	    if (*s != 'T')
		++s;
	    if (*++s) {
		v = scan_oct(s, 2, &numlen);
		if (numlen == 0)
		    v = 1;
	    }
	    rb_set_safe_level(v);
	}
	else {
	    while (s && *s) {
		if (*s == '-') {
		    s++;
		    if (ISSPACE(*s)) {
			do {
			    s++;
			} while (ISSPACE(*s));
			continue;
		    }
		}
		if (!*s)
		    break;
		if (!strchr("EIdvwWrK", *s))
		    rb_raise(rb_eRuntimeError,
			     "invalid switch in RUBYOPT: -%c", *s);
		s = moreswitches(s, opt);
	    }
	}
	if (src_enc_name)
	    opt->src.enc.name = src_enc_name;
	if (ext_enc_name)
	    opt->ext.enc.name = ext_enc_name;
    }

    if (opt->version) {
	ruby_show_version();
	return Qtrue;
    }
    if (opt->copyright) {
	ruby_show_copyright();
    }

    if (rb_safe_level() >= 4) {
	OBJ_TAINT(rb_argv);
	OBJ_TAINT(GET_VM()->load_path);
    }

    if (!opt->e_script) {
	if (argc == 0) {	/* no more args */
	    if (opt->verbose)
		return Qtrue;
	    opt->script = "-";
	}
	else {
	    opt->script = argv[0];
	    if (opt->script[0] == '\0') {
		opt->script = "-";
	    }
	    else if (opt->do_search) {
		char *path = getenv("RUBYPATH");

		opt->script = 0;
		if (path) {
		    opt->script = dln_find_file_r(argv[0], path, fbuf, sizeof(fbuf));
		}
		if (!opt->script) {
		    opt->script = dln_find_file_r(argv[0], getenv(PATH_ENV), fbuf, sizeof(fbuf));
		}
		if (!opt->script)
		    opt->script = argv[0];
	    }
	    argc--;
	    argv++;
	}
    }

    ruby_script(opt->script);
#if defined DOSISH || defined __CYGWIN__
    translate_char(RSTRING_PTR(rb_progname), '\\', '/');
#endif
    GC_WB(&opt->script_name, rb_str_new4(rb_progname));
    opt->script = RSTRING_PTR(opt->script_name);
    ruby_set_argv(argc, argv);
    process_sflag(opt);

    ruby_init_loadpath();
    safe = rb_safe_level();
    rb_set_safe_level_force(0);
    ruby_init_gems(!(opt->disable & DISABLE_BIT(gems)));
    lenc = rb_locale_encoding();
#if !WITH_OBJC
    for (i = 0; i < RARRAY_LEN(rb_argv); i++) {
	rb_enc_associate(RARRAY_PTR(rb_argv)[i], lenc);
    }
#endif
    parser = rb_parser_new();
    if (opt->yydebug) rb_parser_set_yydebug(parser, Qtrue);
    if (opt->ext.enc.name != 0) {
#if WITH_OBJC
	opt->ext.enc.enc = opt_enc_find(opt->ext.enc.name);
#else
	opt->ext.enc.index = opt_enc_index(opt->ext.enc.name);
#endif
    }
    if (opt->src.enc.name != 0) {
#if WITH_OBJC
	opt->src.enc.enc = opt_enc_find(opt->src.enc.name);
	src_encoding = opt->src.enc.enc;
#else
	opt->src.enc.index = opt_enc_index(opt->src.enc.name);
	src_encoding_index = opt->src.enc.index;
#endif
    }
#if WITH_OBJC
    if (opt->ext.enc.enc != NULL) {
	enc = opt->ext.enc.enc;
    }
#else
    if (opt->ext.enc.index >= 0) {
	enc = rb_enc_from_index(opt->ext.enc.index);
    }
#endif
    else {
	enc = lenc;
    }
    rb_enc_set_default_external(rb_enc_from_encoding(enc));

    rb_set_safe_level_force(safe);
    if (opt->e_script) {
	rb_encoding *eenc;
#if WITH_OBJC
	if (opt->src.enc.enc != NULL) {
	    eenc = opt->src.enc.enc;
	}
#else
	if (opt->src.enc.index >= 0) {
	    eenc = rb_enc_from_index(opt->src.enc.index);
	}
#endif
	else {
	    eenc = lenc;
	}
#if !WITH_OBJC
	rb_enc_associate(opt->e_script, eenc);
#endif
	require_libraries();
	tree = rb_parser_compile_string(parser, opt->script, opt->e_script, 1);
    }
    else {
	if (opt->script[0] == '-' && !opt->script[1]) {
	    forbid_setid("program input from stdin");
	}
	tree = load_file(parser, opt->script, 1, opt);
    }

    if (!tree) return Qfalse;

    process_sflag(opt);
    opt->xflag = 0;

    if (rb_safe_level() >= 4) {
#if __LP64__
	RCLASS_RC_FLAGS(rb_argv) &= ~FL_TAINT;
	RCLASS_RC_FLAGS(GET_VM()->load_path) &= ~FL_TAINT;
#else
	FL_UNSET(rb_argv, FL_TAINT);
	FL_UNSET(GET_VM()->load_path, FL_TAINT);
#endif
    }

    if (opt->do_check) {
	printf("Syntax OK\n");
	return Qtrue;
    }

    if (opt->do_print) {
	tree = rb_parser_append_print(parser, tree);
    }
    if (opt->do_loop) {
	tree = rb_parser_while_loop(parser, tree, opt->do_line, opt->do_split);
    }

    iseq = rb_iseq_new(tree, rb_str_new2("<main>"),
		       opt->script_name, Qfalse, ISEQ_TYPE_TOP);

    if (opt->dump & DUMP_BIT(insns)) {
	rb_io_write(rb_stdout, ruby_iseq_disasm(iseq));
	rb_io_flush(rb_stdout);
	return Qtrue;
    }

    return iseq;
}

static NODE *
load_file(VALUE parser, const char *fname, int script, struct cmdline_options *opt)
{
    extern VALUE rb_stdin;
    VALUE f;
    int line_start = 1;
    NODE *tree = 0;
    rb_encoding *enc;

    if (!fname)
	rb_load_fail(fname);
    if (strcmp(fname, "-") == 0) {
	f = rb_stdin;
    }
    else {
	int fd, mode = O_RDONLY;
#if defined DOSISH || defined __CYGWIN__
	{
	    const char *ext = strrchr(fname, '.');
	    if (ext && STRCASECMP(ext, ".exe") == 0)
		mode |= O_BINARY;
	}
#endif
	if ((fd = open(fname, mode)) < 0) {
	    rb_load_fail(fname);
	}

	f = rb_io_fdopen(fd, mode, fname);
    }

    if (script) {
	VALUE c = 1;		/* something not nil */
	VALUE line;
	char *p;
	int no_src_enc = !opt->src.enc.name;
	int no_ext_enc = !opt->ext.enc.name;

#if !WITH_OBJC
	enc = rb_usascii_encoding();
	rb_funcall(f, rb_intern("set_encoding"), 1, rb_enc_from_encoding(enc));
#endif

	if (opt->xflag) {
	    forbid_setid("-x");
	    opt->xflag = Qfalse;
	    while (!NIL_P(line = rb_io_gets(f))) {
		line_start++;
		const char *lineptr = RSTRING_PTR(line);
		if (RSTRING_LEN(line) > 2
		    && lineptr[0] == '#'
		    && lineptr[1] == '!') {
		    if ((p = strstr(lineptr, "ruby")) != 0) {
			goto start_read;
		    }
		}
	    }
	    rb_raise(rb_eLoadError, "no Ruby script found in input");
	}

	c = rb_io_getbyte(f);
	if (c == INT2FIX('#')) {
	    c = rb_io_getbyte(f);
	    if (c == INT2FIX('!')) {
		line = rb_io_gets(f);
		if (NIL_P(line))
		    return 0;

		if ((p = strstr(RSTRING_PTR(line), "ruby")) == 0) {
		    /* not ruby script, kick the program */
		    char **argv;
		    char *path;
		    char *pend = RSTRING_BYTEPTR(line) + RSTRING_BYTELEN(line);

		    p = RSTRING_BYTEPTR(line);	/* skip `#!' */
		    if (pend[-1] == '\n')
			pend--;	/* chomp line */
		    if (pend[-1] == '\r')
			pend--;
		    *pend = '\0';
		    while (p < pend && ISSPACE(*p))
			p++;
		    path = p;	/* interpreter path */
		    while (p < pend && !ISSPACE(*p))
			p++;
		    *p++ = '\0';
		    if (p < pend) {
			argv = ALLOCA_N(char *, origarg.argc + 3);
			argv[1] = p;
			MEMCPY(argv + 2, origarg.argv + 1, char *, origarg.argc);
		    }
		    else {
			argv = origarg.argv;
		    }
		    argv[0] = path;
		    execv(path, argv);

		    rb_fatal("Can't exec %s", path);
		}

	      start_read:
		p += 4;
		RSTRING_BYTEPTR(line)[RSTRING_BYTELEN(line) - 1] = '\0';
		if (RSTRING_BYTEPTR(line)[RSTRING_BYTELEN(line) - 2] == '\r')
		    RSTRING_BYTEPTR(line)[RSTRING_BYTELEN(line) - 2] = '\0';
		if ((p = strstr(p, " -")) != 0) {
		    p++;	/* skip space before `-' */
		    while (*p == '-') {
			p = moreswitches(p + 1, opt);
		    }
		}

		/* push back shebang for pragma may exist in next line */
		rb_io_ungetc(f, rb_str_new2("!\n"));
	    }
	    else if (!NIL_P(c)) {
		rb_io_ungetc(f, c);
	    }
	    rb_io_ungetc(f, INT2FIX('#'));
	    if (no_src_enc && opt->src.enc.name) {
#if WITH_OBJC
		opt->src.enc.enc = opt_enc_find(opt->src.enc.name);
		src_encoding = opt->src.enc.enc;
#else
		opt->src.enc.index = opt_enc_index(opt->src.enc.name);
		src_encoding_index = opt->src.enc.index;
#endif
	    }
	    if (no_ext_enc && opt->ext.enc.name) {
#if WITH_OBJC
		opt->ext.enc.enc = opt_enc_find(opt->ext.enc.name);
#else
		opt->ext.enc.index = opt_enc_index(opt->ext.enc.name);
#endif
	    }
	}
	else if (!NIL_P(c)) {
	    rb_io_ungetc(f, c);
	}
	require_libraries();	/* Why here? unnatural */
    }
#if WITH_OBJC
    if (opt->src.enc.enc != NULL) {
    	enc = opt->src.enc.enc;
    }
    else {
	enc = rb_locale_encoding();
    }
#else
    if (opt->src.enc.index >= 0) {
	enc = rb_enc_from_index(opt->src.enc.index);
    }
    else if (f == rb_stdin) {
	enc = rb_locale_encoding();
    }
    else {
	enc = rb_usascii_encoding();
    }
#endif
    rb_funcall(f, rb_intern("set_encoding"), 1, rb_enc_from_encoding(enc));
    tree = (NODE *)rb_parser_compile_file(parser, fname, f, line_start);
    rb_funcall(f, rb_intern("set_encoding"), 1, rb_parser_encoding(parser));
    if (script && rb_parser_end_seen_p(parser)) {
	rb_define_global_const("DATA", f);
    }
    else if (f != rb_stdin) {
	rb_io_close(f);
    }
    return tree;
}

void *
rb_load_file(const char *fname)
{
    struct cmdline_options opt;

    MEMZERO(&opt, opt, 1);
#if WITH_OBJC
    opt.src.enc.enc = src_encoding;
#else
    opt.src.enc.index = src_encoding_index;
#endif
    return load_file(rb_parser_new(), fname, 0, &opt);
}

#if !defined(PSTAT_SETCMD) && !defined(HAVE_SETPROCTITLE)
#if !defined(_WIN32) && !(defined(HAVE_SETENV) && defined(HAVE_UNSETENV))
#define USE_ENVSPACE_FOR_ARG0
#endif

#ifdef USE_ENVSPACE_FOR_ARG0
extern char **environ;
#endif

static int
get_arglen(int argc, char **argv)
{
    char *s = argv[0];
    int i;

    if (!argc) return 0;
    s += strlen(s);
    /* See if all the arguments are contiguous in memory */
    for (i = 1; i < argc; i++) {
	if (argv[i] == s + 1) {
	    s++;
	    s += strlen(s);	/* this one is ok too */
	}
	else {
	    break;
	}
    }
#if defined(USE_ENVSPACE_FOR_ARG0)
    if (environ && (s == environ[0])) {
	s += strlen(s);
	for (i = 1; environ[i]; i++) {
	    if (environ[i] == s + 1) {
		s++;
		s += strlen(s);	/* this one is ok too */
	    }
	}
	ruby_setenv("", NULL); /* duplicate environ vars */
    }
#endif
    return s - argv[0];
}
#endif

static void
set_arg0(VALUE val, ID id)
{
    const char *s;
    long i;

    if (origarg.argv == 0)
	rb_raise(rb_eRuntimeError, "$0 not initialized");
    StringValue(val);
    s = RSTRING_PTR(val);
    i = RSTRING_LEN(val);
#if defined(PSTAT_SETCMD)
    if (i > PST_CLEN) {
	union pstun un;
	char buf[PST_CLEN + 1];	/* PST_CLEN is 64 (HP-UX 11.23) */
	strncpy(buf, s, PST_CLEN);
	buf[PST_CLEN] = '\0';
	un.pst_command = buf;
	pstat(PSTAT_SETCMD, un, PST_CLEN, 0, 0);
    }
    else {
	union pstun un;
	un.pst_command = s;
	pstat(PSTAT_SETCMD, un, i, 0, 0);
    }
#elif defined(HAVE_SETPROCTITLE)
    setproctitle("%.*s", (int)i, s);
#else

    if (i >= origarg.len) {
	i = origarg.len;
    }

    memcpy(origarg.argv[0], s, i);

    {
	int j;
	char *t = origarg.argv[0] + i;
	*t = '\0';

	if (i + 1 < origarg.len) memset(t + 1, ' ', origarg.len - i - 1);
	for (j = 1; j < origarg.argc; j++) {
	    origarg.argv[j] = t;
	}
    }
#endif
    rb_progname = rb_tainted_str_new(s, i);
}

void
ruby_script(const char *name)
{
    if (name) {
	rb_progname = rb_tainted_str_new2(name);
    }
}

static int uid, euid, gid, egid;

static void
init_ids(void)
{
    uid = (int)getuid();
    euid = (int)geteuid();
    gid = (int)getgid();
    egid = (int)getegid();
#ifdef VMS
    uid |= gid << 16;
    euid |= egid << 16;
#endif
    if (uid && (euid != uid || egid != gid)) {
	rb_set_safe_level(1);
    }
}

static void
forbid_setid(const char *s)
{
    if (euid != uid)
        rb_raise(rb_eSecurityError, "no %s allowed while running setuid", s);
    if (egid != gid)
        rb_raise(rb_eSecurityError, "no %s allowed while running setgid", s);
    if (rb_safe_level() > 0)
        rb_raise(rb_eSecurityError, "no %s allowed in tainted mode", s);
}

static void
verbose_setter(VALUE val, ID id, VALUE *variable)
{
    ruby_verbose = RTEST(val) ? Qtrue : val;
}

static VALUE
opt_W_getter(VALUE val, ID id)
{
    if (ruby_verbose == Qnil)
	return INT2FIX(0);
    if (ruby_verbose == Qfalse)
	return INT2FIX(1);
    if (ruby_verbose == Qtrue)
	return INT2FIX(2);
    return Qnil;		/* not reached */
}

void
ruby_prog_init(void)
{
    init_ids();

    rb_define_hooked_variable("$VERBOSE", &ruby_verbose, 0, verbose_setter);
    rb_define_hooked_variable("$-v", &ruby_verbose, 0, verbose_setter);
    rb_define_hooked_variable("$-w", &ruby_verbose, 0, verbose_setter);
    rb_define_virtual_variable("$-W", opt_W_getter, 0);
    rb_define_variable("$DEBUG", &ruby_debug);
    rb_define_variable("$-d", &ruby_debug);

    rb_define_hooked_variable("$0", &rb_progname, 0, set_arg0);
    rb_define_hooked_variable("$PROGRAM_NAME", &rb_progname, 0, set_arg0);
    GC_ROOT(&rb_progname);

    rb_define_global_const("ARGV", rb_argv);
    rb_global_variable(&rb_argv0);

#ifdef MSDOS
    /*
     * There is no way we can refer to them from ruby, so close them to save
     * space.
     */
    (void)fclose(stdaux);
    (void)fclose(stdprn);
#endif
}

void
ruby_set_argv(int argc, char **argv)
{
    int i;
    VALUE av = rb_argv;

#if defined(USE_DLN_A_OUT)
    if (origarg.argv)
	dln_argv0 = origarg.argv[0];
    else
	dln_argv0 = argv[0];
#endif
    rb_ary_clear(av);
    for (i = 0; i < argc; i++) {
	VALUE arg = rb_tainted_str_new2(argv[i]);

	OBJ_FREEZE(arg);
	rb_ary_push(av, arg);
    }
}

static VALUE
false_value(void)
{
    return Qfalse;
}

static VALUE
true_value(void)
{
    return Qtrue;
}

#define rb_define_readonly_boolean(name, val) \
    rb_define_virtual_variable((name), (val) ? true_value : false_value, 0)

void *
ruby_process_options(int argc, char **argv)
{
    struct cmdline_arguments *args;
    struct cmdline_options *opt;
    NODE *tree;

    args = (struct cmdline_arguments *)xmalloc(sizeof(struct cmdline_arguments));
    opt = (struct cmdline_options *)xmalloc(sizeof(struct cmdline_options));

    MEMZERO(opt, opt, 1);
    ruby_script(argv[0]);	/* for the time being */
    rb_argv0 = rb_progname;
    args->argc = argc;
    args->argv = argv;
    args->opt = opt;
    opt->src.enc.enc = src_encoding;
    opt->ext.enc.enc = NULL;
    tree = (NODE *)rb_vm_call_cfunc(rb_vm_top_self(),
				    process_options, (VALUE)args,
				    0, rb_progname);

    rb_define_readonly_boolean("$-p", opt->do_print);
    rb_define_readonly_boolean("$-l", opt->do_line);
    rb_define_readonly_boolean("$-a", opt->do_split);

    return tree;
}

void
ruby_sysinit(int *argc, char ***argv)
{
#if defined(__APPLE__) && (defined(__MACH__) || defined(__DARWIN__))
    int i, n = *argc, len = 0;
    char **v1 = *argv, **v2, *p;

    for (i = 0; i < n; ++i) {
	len += strlen(v1[i]) + 1;
    }
    v2 = malloc((n + 1)* sizeof(char*) + len);
    p = (char *)&v2[n + 1];
    for (i = 0; i < n; ++i) {
	int l = strlen(v1[i]);
	memcpy(p, v1[i], l + 1);
	v2[i] = p;
	p += l + 1;
    }
    v2[n] = 0;
    *argv = v2;
#elif defined(__MACOS__) && defined(__MWERKS__)
    *argc = ccommand(argv);
#elif defined(_WIN32)
    void rb_w32_sysinit(int *argc, char ***argv);
    rb_w32_sysinit(argc, argv);
#endif
    origarg.argc = *argc;
    origarg.argv = *argv;
#if !defined(PSTAT_SETCMD) && !defined(HAVE_SETPROCTITLE)
    origarg.len = get_arglen(origarg.argc, origarg.argv);
#endif
#if defined(USE_DLN_A_OUT)
    dln_argv0 = origarg.argv[0];
#endif
}
