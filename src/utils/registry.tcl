# usage: tclsh registry.tcl destination
# e.g. tclsh registry.tcl "c:/bitkeeper"

proc main {} \
{
	global argv options reglog shortcutlog tcl_platform
	
	set options(shellx_network) 0
	set options(shellx_local) 0
	set options(bkscc) 0
	while {[string match {-*} [lindex $argv 0]]} {
		set option [lindex $argv 0]
		set argv [lrange $argv 1 end]
		switch -exact -- $option {
			-n	{set options(shellx_network) 1}
			-l	{set options(shellx_local) 1}
			-s	{set options(bkscc) 1}
			default {
				puts stderr "unknown option \"$option\""
				exit 1
			}
		}
	}
	set destination [file nativename [lindex $argv 0]]
	# 95 == 98 in tcl.
	if {$tcl_platform(os) == "Windows 95"} {
		set destination [shortname $destination]
	}
	set bk [file join $destination bk.exe]
	if {![file exists $bk]} {
		puts stderr "can't find a usable bk.exe in $destination"
		exit 1
	}

	set reglog {}
	set shortcutlog {}
	if {[catch {registry_install $destination}]} {
		# failed, almost certainly because user doesn't have
		# admin privs. Whatever the reason we can still do
		# the startmenu and path stuff for this user
		startmenu_install $destination
		addpath user $destination
		set exit 2
	} else {
		# life is good; registry was updated
		startmenu_install $destination
		addpath system $destination
		set exit 0
	}

	writelog $destination

	exit $exit
}

proc registry_install {destination} \
{
	global env reglog options

	set bk [file join $destination bk.exe]
	catch {exec $bk version -s} version
	set id "bk-$version"
	set dll [shortname $destination/bkscc.dll]
	
        # N.B. the command 'reg' has a side effect of adding each key
        # to a global array we can later write to a log...
        # empty keys are created so they get logged appropriately
	set HKLMS "HKEY_LOCAL_MACHINE\\Software"
	set MWC "Microsoft\\Windows\\CurrentVersion"
        reg set $HKLMS\\bitmover
        reg set $HKLMS\\bitmover\\bitkeeper
	reg set $HKLMS\\bitmover\\bitkeeper installdir $destination
	reg set $HKLMS\\bitmover\\bitkeeper rel $id
	if {$options(bkscc)} {
		reg set $HKLMS\\bitmover\\bitkeeper SCCServerName BitKeeper
		reg set $HKLMS\\bitmover\\bitkeeper SCCserverPath $dll
		reg set $HKLMS\\SourceCodeControlProvider ProviderRegkey \
		    "Software\\bitmover\\bitkeeper"
	}
        reg set $HKLMS\\bitmover\\bitkeeper\\shellx
	reg set $HKLMS\\bitmover\\bitkeeper\\shellx networkDrive \
	    $options(shellx_network)
	reg set $HKLMS\\bitmover\\bitkeeper\\shellx LocalDrive \
	    $options(shellx_local)
	reg set $HKLMS\\$MWC\\App\ Management\\ARPCache\\$id
	reg set $HKLMS\\$MWC\\Uninstall\\$id
	reg set $HKLMS\\$MWC\\Uninstall\\$id DisplayName "BitKeeper $version"
	reg set $HKLMS\\$MWC\\Uninstall\\$id DisplayVersion $version
	reg set $HKLMS\\$MWC\\Uninstall\\$id Publisher "BitMover, Inc."
	# store the short name, because the uninstall code has a hack
	# that assumes the name of the executable doesn't have a space. 
	# Also need to use / rather than \ because we may execute this
	# in an msys shell.
	reg set $HKLMS\\$MWC\\Uninstall\\$id UninstallString \
	    "[shortname $destination]\\bkuninstall -S \"$destination\\install.log\""
	reg set $HKLMS\\$MWC\\Uninstall\\$id URLInfoAbout \
		 "http://www.bitkeeper.com"
	reg set $HKLMS\\$MWC\\Uninstall\\$id HelpLink \
		 "http://www.bitkeeper.com/Support.html"

}

proc startmenu_install {dest {group "BitKeeper"}} \
{
	global env shortcutlog tcl_platform

	set bk [file join $dest bk.exe]
	set uninstall [file join $dest bkuninstall.exe]
	set installLog [file join $dest install.log]

	# 95 == 98 in tcl.
	if {$tcl_platform(os) == "Windows 95"} {
		set dest [shortname $dest]
		set bk [shortname $bk]
		set uninstall [shortname $uninstall]
		set installLog [shortname $installLog]
	}

	lappend shortcutlog "CreateGroup \"$group\""
	# by not specifying whether this is a common or user group 
	# it will default to common if the user has admin privs and
	# user if not.
	progman CreateGroup "$group,"
	progman AddItem "$bk helptool,BitKeeper Documentation,,,,,,,1"
	progman AddItem "$bk sendbug,Submit bug report,,,,,,,1"
	progman AddItem "$bk support,Request BitKeeper Support,,,,,,,1"
	if {$tcl_platform(os) == "Windows 95"} {
		progman AddItem "control appwiz.cpl,Uninstall BitKeeper,,,,,C:\\,,1"
	} else {
		progman AddItem "$uninstall -S \"$installLog\",Uninstall BitKeeper,,,,,C:\\,,1"
	}
	progman AddItem "$dest\\bk_refcard.pdf,Quick Reference,,,,,,,0"
	progman AddItem "$dest\\gnu\\msys.bat,Msys Shell,,,,,,,1"
	progman AddItem "http://www.bitkeeper.com,BitKeeper on the Web,,,,,,,1"
	progman AddItem "http://www.bitkeeper.com/Test.html,BitKeeper Test Drive,,,,,,,1"
}
# use dde to talk to the program manager
proc progman {command details} \
{
	global reglog

	set command "\[$command ($details)\]"

	if {[catch {dde execute PROGMAN PROGMAN $command} error]} {
		lappend reglog "error $error"
	}
}
# perform a registry operation and save pertinent information to
# a log
proc reg {command args} \
{
	global reglog
	if {$command== "set"} {
		set key [lindex $args 0]
		set name [lindex $args 1]
		set value [lindex $args 2]
		if {[llength $args] == 4} {
			set type [lindex $args 3]
			set command [list registry set $key $name $value $type]
		} else {
			set command [list registry set $key $name $value]
		}
		if {$name eq ""} {
			lappend reglog "set $key"
		} else {
			lappend reglog "set $key \[$name\]"
		}
	} elseif {$command == "modify"} {
		set key [lindex $args 0]
		set name [lindex $args 1]
		set value [lindex $args 2]
		set newbits [lindex $args 3]
		if {[catch {set type [registry type $key $name]}]} {
			set type sz
		}
		if {$newbits eq ""} {
			lappend reglog "modify $key \[$name\]"
		} else {
			lappend reglog "modify $key \[$name\] $newbits"
		}
		set command [list registry set $key $name $value $type]
	} else {
		# nothing else gets logged at this point (for example,
		# registry broadcast)
		set command [concat registry $command $args]
	}
	uplevel $command
}

proc writelog {dest} \
{
	global reglog shortcutlog

	# we process the data in reverse order since that is the
	# order in which things must be undone
	set f [open "$dest/registry.log" w]
	while {[llength $reglog] > 0} {
		set item [lindex $reglog end]
		set reglog [lrange $reglog 0 end-1]
		puts $f $item
	}
	close $f

	set f [open "$dest/shortcuts.log" w]
	while {[llength $shortcutlog] > 0} {
		set item [lindex $shortcutlog end]
		set shortcutlog [lrange $shortcutlog 0 end-1]
		puts $f $item
	}
	close $f
}

proc addpath {type dir} \
{
	global	env tcl_platform

	# 95 == 98 in tcl.
	if {$tcl_platform(os) == "Windows 95"} {
		autoexec $dir
		return
	}

	if {$type eq "system"} {
		set key "HKEY_LOCAL_MACHINE\\System\\CurrentControlSet"
		append key "\\Control\\Session Manager\\Environment"
	} else {
		set key "HKEY_CURRENT_USER\\Environment"
	}

	set regcmd "modify"
	if {[catch {set path [registry get $key Path]}]} {
		# it's possible that there won't be a Path value
		# if the key is under HKEY_CURRENT_USER
		set path ""
		set regcmd "set"
	}

	# at this point it's easier to deal with a list of dirs
	# rather than a string of semicolon-separated dirs
	set path [split $path {;}]

	# look through the path to see if this directory is already
	# there (presumably from a previous install); no sense in
	# adding a duplicate

	set npath ""
	foreach d $path {
		if {[shortname $d] eq [shortname $dir]} {
			# dir is already in the path
			return 
		}
		if {![file exists "$d/bkhelp.txt"]} {
			lappend npath $d
		}
	}

	lappend npath $dir
	set path [join $npath {;}]
	if {$regcmd == "set"} {
		reg set $key Path $path expand_sz
	} else {
		# this is going to get logged even though we only modify the
		# key (versus creating it). Andrew wanted to know the exact
		# bits added to the path so we'll pass that info along so 
		# it gets logged
		reg modify $key Path $path $dir
	}
	reg broadcast Environment
}

proc autoexec {dir} \
{
	global	env

	# Try and find autoexec.bat, always look at $COMSPEC first.
	# Nota bene: keep this in sync with bkuninstall.c
	if {![info exists env(COMSPEC)]} {
		set drv "C";
	} else {
		if {[string range $env(COMSPEC) 1 1] != ":"} {
			set drv "C";
		} else {
			set drv [string range $env(COMSPEC) 0 0]
		}
	}
	
	foreach drv {$drv c d e f g h i j k l m n o p q r s t u v w x y z} {
		if {[file exists "$drv:/autoexec.bat"]} { break }
	}
	set lines {}
	if {[file exists "$drv:/autoexec.bat"]} { 
		set fd [open "$drv:/autoexec.bat"]
		while {[gets $fd buf] >= 0} {
			# Throw away the previous entry, if any
			# XXX - does not verify the next line is BK line.
			# Should we save the old autoexec.bat file?
			if {$buf == "REM Added by BitKeeper"} {
				gets $fd buf
				gets $fd buf
				continue
			}
			lappend lines $buf
		}
		close $fd
	} else {
		puts "Could not find autoexec.bat location, assuming C:"
		drv = "C"
	}
	lappend lines "REM Added by BitKeeper"
	lappend lines "REM Remove all three of these lines if you edit by hand"
	lappend lines "SET PATH=\"%PATH%;$dir\""
	set fd [open "$drv:/autoexec.bat" w]
	foreach line $lines {
		puts $fd $line
	}
	close $fd
	puts ""
	puts "Changed path to include $dir in $drv:\\autoexec.bat"
}

proc shortname {dir} \
{
	global	env

	if {[catch {set d1 [file attributes $dir -shortname]}]} {
		return $dir
	}
	if {[catch {set d2 [file nativename $d1]}]} {
		return $d1
	}
	return $d2
}

main
