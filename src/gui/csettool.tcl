# diffrtool - view differences between repositories
# Copyright (c) 1999 by Larry McVoy; All rights reserved
# @(#) csettool.tcl 1.39@(#) akushner@disks.bitmover.com

# If even partially visible, return 1
proc nextFile {} \
{
	global	fileCount lastFile

	if {$lastFile == $fileCount} { return }
	incr lastFile
	dotFile
}

proc redoFile {} \
{
	# Preserve the current view, re-do all the magic, then restore
	# the view
	set view [diffView]
	dotFile
	diffView $view
	
}

proc prevFile {} \
{
	global	lastFile

	if {$lastFile == 1} { return 0 }
	incr lastFile -1
	dotFile
	return 1
}

# XXX: Some functionality that Larry never implemented?
proc nextCset {} \
{
}

proc prevCset {} \
{
}

proc file_history {} \
{
	global	lastFile Files file_start_stop file_stop RealFiles

	set line $Files($lastFile)
	set line "$line.0"
	set file $RealFiles($lastFile)
	if {[regexp "^  $file_start_stop" "$file" dummy file start stop] == 0} {
		regexp "^  $file_stop" "$file" dummy f stop
		set start $stop
		set file "$f"
	}
	catch {exec bk -R revtool -l$stop "$file" &}
}

# Takes a line number as an arg when creating continuations for the file menu
proc dotFile {{line {}}} \
{
	global	lastFile fileCount Files tmp_dir file_start_stop file_stop
	global	RealFiles file finfo
	global gc

	set finfo(lt) ""
	set finfo(rt) ""
	if {$line != ""} { set lastFile $line }
	if {$lastFile == 1} {
		.menu.prevFile configure -state disabled
	} else {
		.menu.prevFile configure -state normal
	}
	if {$lastFile == $fileCount} {
		.menu.nextFile configure -state disabled
	} else {
		.menu.nextFile configure -state normal
	}
	set line $Files($lastFile)
	set line "$line.0"
	.l.filelist.t see $line
	.l.filelist.t tag remove select 1.0 end
	.l.filelist.t tag add select $line "$line lineend + 1 char"
	set file $RealFiles($lastFile)

	clearInfo "Working..."

	# busy is put after we change the selection. This is because busy
	# causes a screen update and we want the selection set quickly to make
	# the user think we're responsive.
	busy 1
	if {[regexp "^  $file_start_stop" "$file" dummy file start stop] == 0} {
		regexp "^  $file_stop" "$file" dummy f stop
		set start $stop
		set file "$f"
	}
	set p [open "| bk prs -hr$start {-d:PARENT:\n} \"$file\""]
	gets $p parent
	catch { close $p }
	if {$parent == ""} { set parent "1.0" }
	set finfo(l) "$file@$parent"
	set finfo(r) "$file@$stop"
	set p [open "| bk prs -hr$parent {-d:T: :Dd::DM::Dy:\n} \"$file\""]
	gets $p finfo(lt)
	catch { close $p }
	set p [open "| bk prs -hr$stop {-d:T: :Dd::DM::Dy:\n} \"$file\""]
	gets $p finfo(rt)
	catch { close $p }
	set tmp [file tail "$file"]
	set l [file join $tmp_dir $tmp-${parent}_[pid]]
	set r [file join $tmp_dir $tmp-${stop}_[pid]]
	if {$::showAnnotations} {
		set annotate "$gc(cset.annotation)"
		if {[string index $annotate 0] != "-"} {
			set annotate "-$annotate"
		}
		if {[string first "a" $annotate] == -1} {
			append annotate "a"
		}
		if {$annotate == "-a"} {set annotate "-aum"}
	} else {
		set annotate ""
	}

	if {$annotate == ""} {
		catch { exec bk get -qkpr$parent "$file" > $l}
		catch { exec bk get -qkpr$stop "$file" > $r}
	} else {
		catch { exec bk get -qkpr$parent $annotate "$file" > $l}
		catch { exec bk get -qkpr$stop $annotate "$file" > $r}
	}

	displayInfo $file $file $parent $stop 
	readFiles $l $r
	catch {file delete $l $r}

	set buf ""
	set line [lindex [split $line "."] 0]
	while {[regexp {^ChangeSet (.*)$} $buf dummy crev] == 0} {
		incr line -1
		set buf [.l.filelist.t get "$line.0" "$line.0 lineend"]
	}
	.l.sccslog.t configure -state normal
	.l.sccslog.t delete 1.0 end

	set dspec \
	    "-d:GFILE: :I: :D: :T: :P:\$if(:HT:){@:HT:}\n\$each(:C:){  (:C:)\n}"
	set prs [open "| bk prs {$dspec} -hr$crev ChangeSet" r]
	set first 1
	while { [gets $prs buf] >= 0 } {
		if {$first == 1} {
			set first 0
			.l.sccslog.t insert end "$buf\n" cset
		} else {
			.l.sccslog.t insert end "$buf\n"
		}
	}
	catch { close $prs }

	set prs [open "| bk prs -bhC$stop {$dspec} \"$file\"" r]
	set save ""
	while { [gets $prs buf] >= 0 } {
		if {$buf == "  "} { continue }
		if {[regexp {^  } $buf]} {
			if {$save != ""} {
				.l.sccslog.t insert end "$save\n" file_tag
				set save ""
			}
			.l.sccslog.t insert end "$buf\n"
		} else {
			# Save it and print it later iff we have comments
			set save $buf
		}
	}
	catch { close $prs }
	while {[.l.sccslog.t get "end - 2 char" end] == "\n\n"} {
		.l.sccslog.t delete "end - 1 char" end
	}
	.l.sccslog.t configure -state disabled
	.l.sccslog.t see end
	.l.sccslog.t xview moveto 0
	busy 0
}

proc getFiles {revs {file_rev {}}} \
{
	global	fileCount lastFile Files line2File file_start_stop
	global  RealFiles fmenu file_old_new bk_fs

	busy 1

	# Only search for the last part of the file. This might fail when 
	# there are multiple file.c@rev in the tree. However, I am trying 
	# to solve the case where csettool is called like 
	# -f~user/some_long_path/src/file.c. The preceding would never 
	# match any of the items in the file list
	set file_rev [file tail $file_rev]

	# Initialize these variables so that files with no differences don't
	# cause failures
        set Diffs(0) 1.0
        set DiffsEnd(0) 1.0

	.l.filelist.t configure -state normal
	.l.filelist.t delete 1.0 end
	set fileCount 0
	set line 0
	set found ""
	set match ""
	if {$revs == "-"} {
		set r "stdin"
	} else {
		set r [open "| bk prs -bhr$revs {-d:I:\n} ChangeSet" r]
	}
	while {[gets $r cset] > 0} {
		.diffs.status.middle configure -text "Getting cset $cset"
		update
		incr line
		.l.filelist.t insert end "ChangeSet $cset\n" cset
		set c [open "| bk cset -Hhr$cset | bk _sort" r]
		while { [gets $c buf] >= 0 } {
			regexp  $file_old_new $buf dummy name oname rev
			if {[string match "1.0" $rev]} continue

			incr fileCount
			incr line
			set line2File($line) $fileCount
			set Files($fileCount) $line

			set RealFiles($fileCount) "  $name@$rev"
			set buf "$oname@$rev"
			if {[string first $file_rev $buf] >= 0} {
				set found $fileCount
			}
			.l.filelist.t insert end "  $buf\n"
			$fmenu(widget) add command -label "$buf" \
			    -command  "dotFile $fileCount"
		}
		catch { close $c }
	}
	catch { close $r }
	if {$fileCount == 0} {
		#displayMessage \ 
		#"ChangeSet doesn't contain files since it is a merge ChangeSet."
		exit
	}
	.l.filelist.t configure -state disabled
	set lastFile 1
	if {$found != ""} {
		dotFile $found
	} else {
		dotFile
	}
	busy 0
}

# --------------- Window stuff ------------------

# the purpose is to clear out all the widgets; typically right before
# filling them back up again.
proc clearInfo {{message ""}} \
{
	.diffs.status.middle configure -text $message
	.diffs.status.l configure -text ""
	.diffs.status.r configure -text ""
	.diffs.left configure -state normal
	.diffs.right configure -state normal
	.l.sccslog.t configure -state normal
	.diffs.left delete 1.0 end
	.diffs.right delete 1.0 end
	.l.sccslog.t delete 1.0 end
	.diffs.left configure -state disabled
	.diffs.right configure -state disabled
	.l.sccslog.t configure -state disabled
}

proc busy {busy} \
{
	set oldCursor [. cget -cursor]
	if {$busy == 1} {
		. configure -cursor watch
		.l.filelist.t configure -cursor watch
		.l.sccslog.t configure -cursor watch
		.diffs.left configure -cursor watch
		.diffs.right configure -cursor watch
	} else {
		. configure -cursor left_ptr
		.l.filelist.t configure -cursor left_ptr
		.l.sccslog.t configure -cursor left_ptr
		.diffs.left configure -cursor left_ptr
		.diffs.right configure -cursor left_ptr
	}
	# only call update if the cursor changes; this will cut down
	# a little bit on the flashing that happens at startup. It doesn't
	# eliminate the problem, but it helps. 
	if {![string match $oldCursor [. cget -cursor]]} {
		update
	}
}

proc pixSelect {x y} \
{
	global	lastFile line2File file

	set line [.l.filelist.t index "@$x,$y"]
	set x [.l.filelist.t get "$line linestart" "$line linestart +2 chars"]
	if {$x != "  "} { return }
	set line [lindex [split $line "."] 0]
	# if we aren't changing which line we're on there's no point in
	# calling dotFile since it is a time consuming process
	if {$line2File($line) == $lastFile} {return}
	set lastFile $line2File($line)
	dotFile
}

proc adjustHeight {diff list} \
{
	global	gc 

	incr gc(cset.listHeight) $list
	.l.filelist.t configure -height $gc(cset.listHeight)
	.l.sccslog.t configure -height $gc(cset.listHeight)
	incr gc(cset.diffHeight) $diff
	.diffs.left configure -height $gc(cset.diffHeight)
	.diffs.right configure -height $gc(cset.diffHeight)
}

proc widgets {} \
{
	global	scroll gc wish tcl_platform d search fmenu app

	getConfig "cset"
	option add *background $gc(BG)
	set gc(bw) 1
	if {$tcl_platform(platform) == "windows"} {
		set gc(py) 0; set gc(px) 1
	} else {
		set gc(py) 1; set gc(px) 4
	}

	frame .l
	frame .l.filelist -background $gc(BG)
	    text .l.filelist.t -height $gc(cset.listHeight) -width 30 \
		-state disabled -wrap none -font $gc(cset.fixedFont) \
		-setgrid true \
		-xscrollcommand { .l.filelist.xscroll set } \
		-yscrollcommand { .l.filelist.yscroll set } \
		-background $gc(cset.listBG) -foreground $gc(cset.textFG)
	    scrollbar .l.filelist.xscroll -wid $gc(cset.scrollWidth) \
		-troughcolor $gc(cset.troughColor) \
		-background $gc(cset.scrollColor) \
		-orient horizontal -command ".l.filelist.t xview"
	    scrollbar .l.filelist.yscroll -wid $gc(cset.scrollWidth) \
		-troughcolor $gc(cset.troughColor) \
		-background $gc(cset.scrollColor) \
		-orient vertical -command ".l.filelist.t yview"
	    grid .l.filelist.t -row 0 -column 0 -sticky news
	    grid .l.filelist.yscroll -row 0 -column 1 -sticky nse 
	    grid .l.filelist.xscroll -row 1 -column 0 -sticky ew
	    grid rowconfigure .l.filelist 0 -weight 1
	    grid rowconfigure .l.filelist 1 -weight 0
	    grid columnconfigure .l.filelist 0 -weight 1

	frame .l.sccslog -background $gc(BG)
	    text .l.sccslog.t -height $gc(cset.listHeight) -width 80 \
		-state disabled -wrap none -font $gc(cset.fixedFont) \
		-setgrid true \
		-xscrollcommand { .l.sccslog.xscroll set } \
		-yscrollcommand { .l.sccslog.yscroll set } \
		-background $gc(cset.listBG) -foreground $gc(cset.textFG)
	    scrollbar .l.sccslog.xscroll -wid $gc(cset.scrollWidth) \
		-troughcolor $gc(cset.troughColor) \
		-background $gc(cset.scrollColor) \
		-orient horizontal -command ".l.sccslog.t xview"
	    scrollbar .l.sccslog.yscroll -wid $gc(cset.scrollWidth) \
		-troughcolor $gc(cset.troughColor) \
		-background $gc(cset.scrollColor) \
		-orient vertical -command ".l.sccslog.t yview"
	    grid .l.sccslog.t -row 0 -column 0 -sticky news
	    grid .l.sccslog.yscroll -row 0 -column 1 -sticky ns
	    grid .l.sccslog.xscroll -row 1 -column 0 -sticky ew
	    grid rowconfigure .l.sccslog 0 -weight 1
	    grid rowconfigure .l.sccslog 1 -weight 0
	    grid columnconfigure .l.sccslog.yscroll 1 -weight 0
	    grid columnconfigure .l.sccslog.xscroll 0 -weight 1
	    grid columnconfigure .l.sccslog.t 0 -weight 1
	    grid columnconfigure .l.sccslog 0 -weight 1

	    createDiffWidgets .diffs

image create photo prevImage \
    -format gif -data {
R0lGODdhDQAQAPEAAL+/v5rc82OkzwBUeSwAAAAADQAQAAACLYQPgWuhfIJ4UE6YhHb8WQ1u
WUg65BkMZwmoq9i+l+EKw30LiEtBau8DQnSIAgA7
}
image create photo nextImage \
    -format gif -data {
R0lGODdhDQAQAPEAAL+/v5rc82OkzwBUeSwAAAAADQAQAAACLYQdpxu5LNxDIqqGQ7V0e659
XhKKW2N6Q2kOAPu5gDDU9SY/Ya7T0xHgTQSTAgA7
}
	set menuwid 7
	frame .menu -background $gc(BG)
	    button .menu.prevCset -font $gc(cset.buttonFont) \
		-bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) \
		-text "<< Cset" -command prevCset
	    button .menu.nextCset -font $gc(cset.buttonFont) \
		-bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) \
		-text ">> Cset" -command nextCset
	    button .menu.prevFile -font $gc(cset.buttonFont) \
		-bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) \
		-image prevImage -command prevFile
	    menubutton .menu.fmb -font $gc(cset.buttonFont) -relief raised \
	        -indicatoron 1 \
		-bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) \
		-text "File" -width 8 -state normal \
		-menu .menu.fmb.menu
		set fmenu(widget) [menu .menu.fmb.menu]
	    $fmenu(widget) add checkbutton \
	        -label "Show Annotations" \
	        -onvalue 1 \
	    	-offvalue 0 \
	        -variable showAnnotations \
	        -command redoFile
	    $fmenu(widget) add separator
	    button .menu.nextFile -font $gc(cset.buttonFont) \
		-bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) \
		-image nextImage -command nextFile
	    button .menu.prev -font $gc(cset.buttonFont) \
		-bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) \
		-image prevImage -state disabled \
		-command {
			searchreset
		    	prev
		}
	    button .menu.next -font $gc(cset.buttonFont) \
		-bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) \
		-image nextImage -state disabled \
		-command {
			searchreset
			next
		}
	    menubutton .menu.mb -font $gc(cset.buttonFont) -relief raised \
	        -indicatoron 1 \
		-bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) \
		-text "History" -width 8 -state normal \
		-menu .menu.mb.menu
		set m [menu .menu.mb.menu]
		$m add command -label "Changeset History" \
		    -command "exec bk revtool &"
		$m add command -label "File History" \
		    -command file_history
	    button .menu.quit -font $gc(cset.buttonFont) \
		-bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) \
		-text "Quit" -command exit 
	    button .menu.help -bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) \
		-font $gc(cset.buttonFont) -text "Help" \
		-command { exec bk helptool csettool & }
	    button .menu.dot -bg $gc(cset.buttonColor) \
		-pady $gc(py) -padx $gc(px) -borderwid $gc(bw) -width 15\
		-font $gc(cset.buttonFont) -text "Current diff" \
		-command dot

	    pack .menu.quit -side left -fill y
	    pack .menu.help -side left -fill y
	    pack .menu.mb -side left -fill y
	    pack .menu.prevFile -side left -fill y
	    pack .menu.fmb -side left -fill y
	    pack .menu.nextFile -side left -fill y
	    pack .menu.prev -side left -fill y
	    pack .menu.dot -side left -fill y
	    pack .menu.next -side left -fill y
	    # Add the search widgets to the menu bar
	    search_widgets .menu .diffs.right

	    # We put status info in the diff status window that is larger
	    # than that expeced by difflib; so, make the label a bit wider 
	    # to keep the display from jiggling
	    .diffs.status.middle configure -width 25
	
	# smaller than this doesn't look good.
	#wm minsize . $x 400

	grid .menu -row 0 -column 0 -sticky w
	grid .l -row 1 -column 0 -sticky nsew
	grid .l.sccslog -row 0 -column 1 -sticky nsew
	grid .l.filelist -row 0 -column 0 -sticky nsew
	grid .diffs -row 2 -column 0 -sticky nsew
	grid rowconfigure .menu 0 -weight 0
	grid rowconfigure .diffs 1 -weight 1
	grid rowconfigure . 0 -weight 0
	grid rowconfigure . 1 -weight 0
	grid rowconfigure . 2 -weight 2
	grid columnconfigure . 0 -weight 1
	grid columnconfigure .menu 0 -weight 1
	grid columnconfigure .l 0 -weight 1
	grid columnconfigure .l.filelist 0 -weight 1
	grid columnconfigure .l.sccslog 1 -weight 1
	grid columnconfigure .diffs 0 -weight 1

	#$search(widget) tag configure search \
	#    -background $gc(cset.searchColor) -font $gc(cset.fixedBoldFont)
	keyboard_bindings
	search_keyboard_bindings
	searchreset
	foreach w {.diffs.left .diffs.right} {
		bindtags $w {all Text .}
	}
	computeHeight "diffs"

	.l.filelist.t tag configure select -background $gc(cset.selectColor) \
	    -relief groove -borderwid 1
	.l.filelist.t tag configure cset \
	    -background $gc(cset.listBG) -foreground $gc(cset.textFG)
	.l.sccslog.t tag configure cset \
	    -background $gc(cset.listBG) -foreground $gc(cset.textFG)
	.l.sccslog.t tag configure file_tag -underline true
	. configure -cursor left_ptr
	.l.sccslog.t configure -cursor left_ptr
	.l.filelist.t configure -cursor left_ptr
	.diffs.left configure -cursor left_ptr
	.diffs.right configure -cursor left_ptr
	. configure -background $gc(BG)
	wm deiconify .
	focus .l.filelist
}

# Set up keyboard accelerators.
proc keyboard_bindings {} \
{
	global gc search tcl_platform
	global afterId

	bind all <Control-b> { if {[Page "yview" -1 0] == 1} { break } }
	bind all <Control-f> { if {[Page "yview"  1 0] == 1} { break } }
	bind all <Control-e> { if {[Page "yview"  1 1] == 1} { break } }
	bind all <Control-y> { if {[Page "yview" -1 1] == 1} { break } }
	bind all <Prior> { if {[Page "yview" -1 0] == 1} { break } }
	bind all <Next> { if {[Page "yview" 1 0] == 1} { break } }
	bind all <Up> { if {[Page "yview" -1 1] == 1} { break } }
	bind all <Down> { if {[Page "yview" 1 1] == 1} { break } }
	bind all <Left> { if {[Page "xview" -1 1] == 1} { break } }
	bind all <Right> { if {[Page "xview" 1 1] == 1} { break } }
	bind all <Home> {
		global	lastDiff

		set lastDiff 1
		dot
		.diffs.left yview -pickplace 1.0
		.diffs.right yview -pickplace 1.0
	}
	bind all <End> {
		global	lastDiff diffCount

		set lastDiff $diffCount
		dot
		.diffs.left yview -pickplace end
		.diffs.right yview -pickplace end
	}
	bind all <Alt-Up> { adjustHeight 1 -1 }
	bind all <Alt-Down> { adjustHeight -1 1 }
	bind all <$gc(cset.quit)>	exit
	bind all <space>		next
	bind all <n>			next
	bind all <p>			prev
	bind all <r>			file_history
	bind all <period>		dot
	bind all <Control-n>		nextFile
	bind all <Control-p>		prevFile

	if {$tcl_platform(platform) == "windows"} {
		bind all <MouseWheel> {
		    if {%D < 0} { next } else { prev }
		}
	} else {
		bind all <Button-4>	prev
		bind all <Button-5>	next
	}
	# note that the "after" is required for windows. Without
	# it we often never see the double-1 events. 
	bind .l.filelist.t <1> { 
		set afterId \
		    [after idle [list after $gc(cset.doubleclick) \
				     pixSelect %x %y]]
		break
	}
	# the idea is, if we detect a double click we'll cancel the 
	# single click, then make sure we perform the single and double-
	# click actions in order
	bind .l.filelist.t <Double-1> {
		if {[info exists afterId]} {
			after cancel $afterId
		}
		pixSelect %x %y
		file_history
		break
	}
	# In the search window, don't listen to "all" tags.
	bindtags $search(text) { .menu.search Entry . }
}

proc main {} \
{
	global argv0 argv argc app showAnnotations gc

	wm title . "Cset Tool"

	# Set 'app' so that the difflib code knows which global config
	# vars to read
	set revs ""
	set argindex 0
	set file_rev ""
	set stdin 0

	while {$argindex < $argc} {
		set arg [lindex $argv $argindex]
		switch -regexp -- $arg {
		    "^-f.*" {
			set ftmp [lindex $argv $argindex]
		   	regexp {^[ \t]*-f(.*)} $ftmp dummy file_rev
		    }
		    "^-r.*" {
			set rev [lindex $argv $argindex]
		   	regexp {^[ \t]*-r(.*)} $rev dummy revs
		    }
		    "^-$" {
			set stdin 1
		    }
		}
		incr argindex
	}
	if {(($revs != "") || ($file_rev != "")) && $stdin} {
		wm withdraw .
		displayMessage "Can't use '-' option with any other options"
		exit
	}
	if {$revs == ""} {
		set revs "+"
	}
	#displayMessage "csetttool: revs=($revs) file=($file_rev)"
	bk_init
	cd2root [file dirname $file_rev]
	if {$stdin == 0} {
		set dspec "-d\$if(:Li: -gt 0){(:I:)\n}"
		set fd [open "| bk prs -hr$revs {$dspec} ChangeSet" r]
		# Only need to read first line to know whether there is content
		gets $fd prs
		if {$prs == ""} {
			catch {wm withdraw .}
			displayMessage "This ChangeSet is a merge ChangeSet and does not contain any files."
			exit
		}
		catch {close $fd}
	}

	loadState
	restoreGeometry cset

	widgets

	if {$gc(cset.annotation) != ""} {
		set showAnnotations 1
	}

	if {$stdin == 1} {
		getFiles "-"
	} else {
		getFiles $revs $file_rev
	}

	bind . <Destroy> {
		if {[string match "." %W]} {
			saveState
		}
	}

	after idle [list wm deiconify .]
}

proc loadState {} \
{
	global State

	catch {::appState load cset State}

}

proc saveState {} \
{
	global State

	# Copy state to a temporary variable, the re-load in the
	# state file in case some other process has updated it
	# (for example, setting the geometry for a different
	# resolution). Then add in the geometry information unique
	# to this instance.
	array set tmp [array get State]
	catch {::appState load cset tmp}
	set res [winfo screenwidth .]x[winfo screenheight .]
	set tmp(geometry@$res) [wm geometry .]

	# Generally speaking, errors at this point are no big
	# deal. It's annoying we can't save state, but it's no 
	# reason to stop running. So, a message to stderr is 
	# probably sufficient. Plus, given we may have been run
	# from a <Destroy> event on ".", it's too late to pop
	# up a message dialog.
	if {[catch {::appState save cset tmp} result]} {
		puts stderr "error writing config file: $result"
	}

}
main
