#!/usr/bin/perl -w

$gperf = '/usr/local/bin/gperf';
$gperf = 'gperf' unless -x $gperf;

$_ = `$gperf --version`;
die "mk-cmd.pl: Requires gperf version >3\n" unless /^GNU gperf 3/;

open(C, "| $gperf > cmd.c.new") or die;

print C <<EOF;
%{
/* !!! automatically generated file !!! Do not edit. */
#include "system.h"
#include "bkd.h"
#include "cmd.h"
%}
%struct-type
%language=ANSI-C
%define lookup-function-name cmd_lookup
%define hash-function-name cmd_hash
%includes

struct CMD;
%%
EOF

open(H, ">cmd.h.new") || die;
print H <<END;
/* !!! automatically generated file !!! Do not edit. */
#ifndef	_CMD_H_
#define	_CMD_H_

enum {
    CMD_UNKNOWN,		/* not a symbol */
    CMD_INTERNAL,		/* internal XXX_main() function */
    CMD_GUI,			/* GUI command */
    CMD_SHELL,			/* shell script in `bk bin` */
    CMD_CPROG,			/* executable in `bk bin` */
    CMD_ALIAS,			/* alias for another symbol */
    CMD_BK_SH,			/* function in bk.script */
    CMD_LSCRIPT,		/* L script */
};

typedef struct CMD {
	char	*name;
	u8	type;		/* type of symbol (from enum above) */
	int	(*fcn)(int, char **);
	char	*alias;		/* name is alias for 'alias' */
	u8	restricted:1;	/* cannot be called from the command line */
	u8	pro:1;		/* only in pro version of bk */
	u8	remote:1;	/* always allowed as a remote command */
} CMD;

CMD	*cmd_lookup(const char *str, unsigned int len);

END

while (<DATA>) {
    chomp;
    s/#.*//;			# ignore comments
    next if /^\s*$/;		# ignore blank lines

    # handle aliases
    if (/(\w+) => (\w+)/) {
	print C "$1, CMD_ALIAS, 0, \"$2\", 0, 0\n";
	next;
    }
    s/\s+$//;			# strict trailing space
    $type = "CMD_INTERNAL";
    $type = "CMD_GUI" if s/\s+gui//;
    $type = "CMD_SHELL" if s/\s+shell//;
    $type = "CMD_CPROG" if s/\s+cprog//;
    $type = "CMD_LSCRIPT" if s/\s+lscript//;

    $r = $pro = $remote = 0;
    $r = 1 if s/\s+restricted//;
    $pro = 1 if s/\s+pro//;
    $remote = 1 if s/\s+remote//;

    if (/\s/) {
	die "Unable to parse mk-cmd.pl line $.: $_\n";
    }

    if ($type eq "CMD_INTERNAL") {
	$m = "${_}_main";
	$m =~ s/^_//;
	print H "int\t$m(int, char **);\n";
    } else {
	$m = 0;
    }
    print C "$_, $type, $m, 0, $r, $pro, $remote\n";
    $rmts{$m} = 1 if $remote;
}
print H "\n#endif\n";
close(H) or die;

# Open bk/src/bk.sh and automatically extract out all shell functions
# and add to the hash table.
open(SH, "bk.sh") || die;
while (<SH>) {
    if (/^_(\w+)\(\)/) {
	print C "$1, CMD_BK_SH, 0, 0, 0, 0\n";
    }
}
close(SH) or die;

# all commands tagged with 'remote' must live in files named bkd_*.c
# (can't use perl's glob() because win32 perl is missing library)
delete $rmts{"sfio_main"};	# Exception to the rules.
open(LS, "bk gfiles bkd_\*.c |") or die;
@ARGV = ();
while (<LS>) {
    chomp;
    push(@ARGV, $_);
}
close(LS) or die;
while (<>) {
    if (/^(\w+_main)\(/) {
	delete $rmts{$1};
    }

    # export bkd command as to the command line
    #  (ex:   cmd_pull_part1 => 'bk _bkd_pull_part1')

    if (/^cmd_(\w+)\(/) {
	print C "_bkd_$1, CMD_INTERNAL, cmd_$1, 0, 0, 0, 1\n";
    }
}
if (%rmts) {
    print STDERR "Commands marked with 'remote' need to move to bkd_*.c:\n";
    foreach (sort keys %rmts) {
	print STDERR "\t$_\n";
    }
    die;
}

close(C) or die;

# only replace cmd.c and cmd.h if they have changed
foreach (qw(cmd.c cmd.h)) {
    if (system("cmp -s $_ $_.new") != 0) {
	rename("$_.new", $_);
    }
    unlink "$_.new";
}


# All the command line functions names in bk should be listed below
# followed by any optional modifiers.  A line with just a single name
# will be an internal C function that calls a XXX_main() function.
# (leading underscores are not included in the _main function)
#
# Modifiers:
#    restricted		can only be called from bk itself
#    pro		only available in the commercial build
#    gui		is a GUI script
#    cprog		is an executable in the `bk bin` directory
#    shell		is a shell script in the `bk bin` directory
#    lscript		is an L script in the `bk bin` directory
#
# Command aliases can be given with this syntax:
#     XXX => YYY
# Where YYY much exist elsewhere in the table.
#
# Order of table doesn't not matter, but please keep builtin functions
# in sorted order.

__DATA__

# builtin functions (sorted)
_g2bk
abort
_access
_adler32
admin
alias
annotate
bam
BAM => bam
base64
bin
bkd
binpool => bam
cat
_catfile	# bsd contrib/cat.c
_cat_partition remote
cfile
changes
check
checked remote
checksum
chksum
clean
_cleanpath
clone
cmdlog
collapse
comments
commit
components	# old compat code
comps
config
cp
partition
create
crypto
cset
csetprune pro
_debugargs remote
deledit
delget
delta
diffs
diffsplit
dotbk
_eula
_exists
export
f2csets
_fgzip
_filtertest1
_filtertest2
_find
_findcset
_findhashdup
findkey remote
fix
fixtool
_fslchmod
_chmod => _fslchmod
_fslcp
_cp => _fslcp
_fslmkdir
_mkdir => _fslmkdir
_fslmv
_mv => _fslmv
_fslrm
_rm => _fslrm
gca
get
gethelp
gethost
_getkv
getmsg
_getopt_test
getuser
glob
gnupatch
gone pro
graft
grep
_gzip
_hashstr_test
_hashfile_test
havekeys remote
_heapdump
help
helpsearch
helptopics
here
_httpfetch
hostme
id remote
idcache
info_server
info_shell
isascii
_isnetwork
inskeys
key2rev
key2path
_keyunlink
_kill
_lconfig
lease
_lease_errtest
legal
level remote
_lines
_link
_listkey restricted
lock
_locktest
log
_logging
_lstat
_mailslot
mailsplit
mail
makepatch
merge
mklock
more
mtime
mv
mvdir
names
ndiff
needscheck
_nested
newroot pro
nfiles
opark
ounpark
parent
park
path
pending
platform
_popensystem
populate
port => pull
_probekey restricted
_progresstest
prompt
prs
_prunekey restricted
pull
push
pwd
r2c
range
rcheck
_rclone
rcs2bk
rcsparse
receive
_recurse
_realpath
regex
_registry
renumber
repair
repogca
repotype
relink
resolve
restore
_reviewmerge
rm
rmdel
_rmshortkeys
root
rset
sane
sccs2bk
_scat
sccslog
send pro
sendbug
set
_setkv
setup
sfiles
_sfiles_bam
_sfiles_clone
sfio remote
_shellSplit_test
shrink
sinfo
smerge
sort
_startmenu
_stat
_stattest
status
stripdel
_strings
_svcinfo
synckeys
tagmerge
takepatch
_tclsh
_testlines
test
testdates
time
tmpdir
_touch
_unbk
undo
undos
unedit
_unittests
_unlink
unlock
uninstall
unpark
unpopulate
unpull
unrm
unwrap
upgrade
users
_usleep
uuencode
uudecode
val
version remote
what
which
xflags
zone

#aliases of builtin functions
add => delta
attach => clone
detach => clone
_cat => _catfile
ci => delta
enter => delta
new => delta
_get => get
co => get
checkout => get
edit => get
comment => comments	# alias for Linus, remove...
identity => id
info => sinfo
_key2path => key2path
_mail => mail
aliases => alias
pager => more
patch => mend
_preference => config
rechksum => checksum
rev2cset => r2c
sccsdiff => diffs
sfind => sfiles
_sort => sort
support => sendbug
_test => test
unget => unedit
user => users

# guis
citool gui
csettool gui
difftool gui
fm3tool gui
fmtool gui
helptool gui
installtool gui
msgtool gui
oldcitool gui
renametool gui
revtool gui
setuptool gui
showproc gui
debugtool gui

# gui aliases
csetool => csettool
fm3 => fm3tool
fm => fmtool
fm2tool => fmtool
histool => revtool
histtool => revtool
sccstool => revtool

# shell scripts
applypatch shell
import shell
resync shell

# c programs
mend cprog
cmp cprog
diff cprog
diff3 cprog
sdiff cprog

# L scripts
hello lscript
