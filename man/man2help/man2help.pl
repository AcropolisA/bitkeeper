
eval "exec perl -Ssw $0 $@"
        if 0;

sub main
{
	$ENV{'GROFF_NO_SGR'} = "no";
	$debug = 0 if 0;
	$m = shift(@ARGV);
	open(FD, $m);
	@macros = <FD>;
	close(FD);
	$prefix = "";
	unless ($ARGV[0] =~ /\./) {
		$prefix = shift(@ARGV);
	}
	$ENV{'GROFF_NO_SGR'} = 1;
	$MAN = "-man";
	unless (system("echo | groff -mgan >/dev/null 2>/dev/null")) {
		$MAN = "-mgan";
	}

	foreach $page (@ARGV) {
		&man2help;
	}
}

# Format the man page, inserting in our help markers and aliases.
sub man2help
{
	($basename = $page) =~ s|.*/||;
	$basename =~ /(.*)\.([^\.]+)$/;
	$name = $1;
	$section = $2;
	$output = "$name-$section.fmt";
	if (-e $output && (stat($output))[9] > (stat($page))[9]) {
		return;
	}
	warn "Format $page ( $name . $section )\n" if $debug;
	open(O, ">$output") || die "open of $output";
	if ($name =~ /^${prefix}(.*)/o) {
		print O "help://$1\n";
		print O "help://$1.$section\n";
	}
	print O "help://$name\n";
	print O "help://$name.$section\n";
	open(D, $page) || die "open of $page";
	open(F, ">tmp");
	print F ".pl 10000i\n";
	print F @macros;
	while (<D>) {
		next if /bk-macros/;
		print F;
		if (m|^\.\\" help://|) {
			s|^.."\s+help://||;
			chop;
			print O "help://$_\n";
		}
	}
	close(D);
	close(F);
	$cmd = "groff -rhelpdoc=1 $MAN -P-u -P-b -Tascii < tmp";
	open(G, "$cmd |");
	$nl = 0;
	while (<G>) {
		if (/^$/) {
			$nl = 1;
			next;
		}
		print O "\n" if ($nl);
		print O;
		$nl = 0;
	}
	print O "\$\n";
	close(O);
	close(G);
}

&main;
