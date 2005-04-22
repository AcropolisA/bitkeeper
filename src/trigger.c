/*
 * Copyright (c) 2001 Larry McVoy & Andrew Chang
 */

#include "bkd.h"
private int localTrigger(char *, char *, char **);
private int remotePreTrigger(char *, char *, char **);
private int remotePostTrigger(char *, char *, char **);

int put_trigger_env(char *where, char *v, char *value);


/*
 * set up the trigger environment
 */
private void
trigger_env(char *prefix, char *event, char *what)
{
	char	buf[100];
	char	*repoid, *lic;

	if (streq("BK", prefix)) {
		put_trigger_env("BK", "SIDE", "client");
		if (lic = licenses_accepted()) {
			safe_putenv("BK_ACCEPTED=%s", lic);
			free(lic);
		}
	} else {
		put_trigger_env("BK", "SIDE", "server");
		put_trigger_env("BK", "HOST", getenv("_BK_HOST"));
		put_trigger_env("BK", "USER", getenv("BK_USER"));
	}
	put_trigger_env(prefix, "HOST", sccs_gethost());
	put_trigger_env(prefix, "USER", sccs_getuser());
	put_trigger_env("BK", "EVENT", event);
	putroot(prefix);
	put_trigger_env(prefix, "TIME_T", bk_time);
	put_trigger_env(prefix, "UTC", bk_utc);
	put_trigger_env(prefix, "VERSION", bk_vers);
	sprintf(buf, "%d", getlevel());
	put_trigger_env(prefix, "LEVEL", buf);
	if (repoid = repo_id()) put_trigger_env(prefix, "REPO_ID", repoid);
	put_trigger_env(prefix, "REALUSER", sccs_realuser());
	put_trigger_env(prefix, "REALHOST", sccs_realhost());
	put_trigger_env(prefix, "PLATFORM", platform());
	if (streq(event, "resolve")) {
		char    pwd[MAXPATH];
		FILE    *f = fopen("BitKeeper/tmp/patch", "r");
		char    *p;

		if (f) {
			buf[0] = 0;
			fnext(buf, f);
			fclose(f);
			assert(buf[0]);
			chomp(buf);
			chdir(RESYNC2ROOT);
			getcwd(pwd, sizeof(pwd));
			chdir(ROOT2RESYNC);
			p = aprintf("%s/%s", pwd, buf);
			put_trigger_env("BK", "PATCH", p);
			free(p);
		}
	}
}

/*
 * trigger:  Fire triggers before and/or after repository level commands.
 *
 * Note: Caller must make sure we are at the package root before
 * calling this function.
 */
int
trigger(char *cmd, char *when)
{
	char	buf[MAXPATH], triggerDir[MAXPATH];
	char	*what, *t, **triggers = 0;
	char	*event = 0;
	int	rc = 0;

	if (getenv("BK_SHOW_TRIGGERS")) {
		ttyprintf("TRIGGER cmd(%s) when(%s)\n", cmd, when);
	}

	if (getenv("BK_NO_TRIGGERS")) return (0);
	/*
	 * For right now, if you set this at all, it means /etc.
	 * In the 3.0 tree, we'll actually respect it.
	 */
	if (getenv("BK_TRIGGER_PATH")) {
		t = strdup("/etc");
	} else unless (t = proj_root(0)) {
		ttyprintf("No root for triggers!\n");
		return (0);
	}
	unless (streq(t, ".")) {
		sprintf(triggerDir, "%s/BitKeeper/triggers", t);
	} else {
		strcpy(triggerDir, "BitKeeper/triggers");
	}

	if (strneq(cmd, "remote pull", 11)) {
		what = "outgoing";
		event = "outgoing pull";
	} else if (strneq(cmd, "push", 4)) {
		what = "outgoing";
		event = "outgoing push";
	} else if (strneq(cmd, "remote clone", 12)) {
		what = "outgoing";
		event = "outgoing clone";
	} else if (strneq(cmd, "remote push", 11)) {
		what = "incoming";
		event = "incoming push";
	} else if (strneq(cmd, "remote rclone", 12)) {
		what = "incoming";
		event = "incoming clone";
	} else if (streq(cmd, "resolve") || streq(cmd, "remote resolve")) {
		what = event = "resolve";
	} else if (strneq(cmd, "clone", 5)) {
		/* XXX - can this happen?  Where's the trigger? */
		what = "incoming";
		event = "incoming clone";
	} else if (strneq(cmd, "_rclone", 6)) {
		what = "outgoing";
		event = "outgoing clone";
	} else if (strneq(cmd, "pull", 4)) {
		what = "incoming";
		event = "incoming pull";
	} else if (strneq(cmd, "apply", 5) || strneq(cmd, "remote apply", 12)) {
		what = event = "apply";
		strcpy(triggerDir, RESYNC2ROOT "/BitKeeper/triggers");
	} else if (strneq(cmd, "commit", 6)) {
		what = event = "commit";
	} else if (strneq(cmd, "merge", 5)) {
		what = event = "commit";
		strcpy(triggerDir, RESYNC2ROOT "/BitKeeper/triggers");
	} else if (strneq(cmd, "delta", 5)) {
		what = event = "delta";
	} else if (strneq(cmd, "tag", 3)) {
		what = event = "tag";
	} else if (strneq(cmd, "fix", 3)) {
		what = event = "fix";
	} else {
		fprintf(stderr,
		    "Warning: Unknown trigger event: %s, ignored\n", cmd);
		return (0);
	}

	unless (isdir(triggerDir)) {
		if (getenv("BK_SHOW_TRIGGERS")) {
			ttyprintf("No trigger dir %s\n", triggerDir);
		}
		return (0);
	}

	/*
	 * XXX - we should see if we need to fork this process before
	 * doing so.  FIXME.
	 */
	sys("bk", "get", "-Sq", triggerDir, SYS);

	/* run post-triggers with a read lock */
	if (streq(when, "post")) repository_downgrade();

	/* post-resolve == post-incoming */
	if (streq(when, "post") && streq(event, "resolve")) what = "incoming";

	/* Run the incoming trigger in the RESYNC dir if there is one.  */
	if (streq(what, "resolve")) {
		strcpy(triggerDir, RESYNC2ROOT "/BitKeeper/triggers");
		assert(isdir(ROOT2RESYNC));
	    	chdir(ROOT2RESYNC);
	}

	/*
	 * Find all the trigger scripts associated with this event.
	 * XXX TODO need to think about the split root case
	 */
	sprintf(buf, "%s-%s", when, what);
	unless (triggers = getTriggers(triggerDir, buf)) {
		if (streq(what, "resolve")) chdir(RESYNC2ROOT);
		if (getenv("BK_SHOW_TRIGGERS")) {
			ttyprintf("No %s triggers in %s \n", buf, triggerDir);
		}
		return (0);
	}

	/*
	 * Run the triggers, they are already sorted by getdir().
	 */
	unless (getenv("BK_STATUS")) putenv("BK_STATUS=UNKNOWN");
	unless (strneq(cmd, "remote ", 7))  {
		rc = localTrigger(event, what, triggers);
	} else if (streq(when, "pre")) {
		rc = remotePreTrigger(event, what, triggers);
	} else 	{
		rc = remotePostTrigger(event, what, triggers);
	}
	freeLines(triggers, free);
	if (streq(what, "resolve")) chdir(RESYNC2ROOT);
	return (rc);
}

/*
 * returns lines array of all filenames in directory that start with
 * the given prefix.
 * The array is sorted.
 */
char	**
getTriggers(char *dir, char *prefix)
{
	int	len = strlen(prefix);
	char	**files;
	char	**lines = 0;
	int	i;

	files = getdir(dir);
	unless (files) return (0);
	EACH (files) {
		int	flen = strlen(files[i]);
		/* skip files that don't match prefix */
		unless (flen >= len && strneq(files[i], prefix, len)) continue;
		/* skip emacs backup files */
		if (files[i][flen - 1] == '~') continue;
		lines = addLine(lines, aprintf("%s/%s", dir, files[i]));
	}
	freeLines(files, free);
	return (lines);
}

private int
runit(char *file, char *what, char *output)
{
	int	status, rc;
	char	*root = streq(what, "resolve") ? RESYNC2ROOT : ".";
	char	*path = strdup(getenv("PATH"));

	safe_putenv("BK_TRIGGER=%s", basenm(file));
	write_log(root, "cmd_log", 0, "Running trigger %s", file);
	if (getenv("BK_SHOW_TRIGGERS")) ttyprintf("Running trigger %s\n", file);
	safe_putenv("PATH=%s", getenv("BK_OLDPATH"));
	status = sysio(0, output, 0, file, SYS);
	safe_putenv("PATH=%s", path);
	free(path);
	if (WIFEXITED(status)) {
		rc = WEXITSTATUS(status);
	} else {
		rc = 100;
	}
	write_log(root, "cmd_log", 0, "Trigger %s returns %d", file, rc);
	if (getenv("BK_SHOWPROC") || getenv("BK_SHOW_TRIGGERS")) {
		ttyprintf("TRIGGER %s => %d\n", basenm(file), rc);
	}
	return (rc);
}

/*
 * Both pre and post client triggers are handled here
 */
private int
localTrigger(char *event, char *what, char **triggers)
{
	int	i, rc = 0;

	trigger_env("BK", event, what);

	EACH(triggers) {
		unless (executable(triggers[i])) continue;
		rc = runit(triggers[i], what, 0);
		if (rc) break;
	}
	return (rc);
}

private int
remotePreTrigger(char *event, char *what, char **triggers)
{
	int	i, rc = 0, lclone = getenv("_BK_LCLONE") != 0;
	char	output[MAXPATH], buf[MAXLINE];
	FILE	*f;

	trigger_env("BKD", event, what);

	bktmp(output, "trigger");
	unless (lclone) fputs("@TRIGGER INFO@\n", stdout);
	EACH(triggers) {
		unless (executable(triggers[i])) continue;
		rc = runit(triggers[i], what, output);
		f = fopen(output, "rt");
		assert(f);
		while (fnext(buf, f)) {
			unless (lclone) printf("%c", BKD_DATA);
			printf("%s", buf);
		}
		fclose(f);
		if (rc) break;
	}
	unless (lclone) {
		printf("%c%d\n", BKD_RC, rc);
		fputs("@END@\n", stdout);
	}
	fflush(stdout);
	unlink(output);
	return (rc);
}
/*
 * This function is called by client side to process the TRIGGER INFO block
 * sent by run_bkd_trigger() above
 */
int
getTriggerInfoBlock(remote *r, int verbose)
{
	char	buf[4096];
	int	i =0, rc = 0;

	while (getline2(r, buf, sizeof(buf)) > 0) {
		if (buf[0] == BKD_DATA) {
			unless (i++) {
				if (verbose) fprintf(stderr,
"------------------------- Remote trigger message --------------------------\n"
				);
			}
			if (verbose) fprintf(stderr, "%s\n", &buf[1]);
		} else if (buf[0] == BKD_RC) {
			rc = atoi(&buf[1]);
			if (rc && r->trace) {
				fprintf(stderr, "trigger failed rc=%d\n", rc);
			}
		}
		if (streq(buf, "@END@")) break;
	}
	if (i && verbose) {
		fprintf(stderr, 
"---------------------------------------------------------------------------\n"
		);
	}
	return (rc);
}

private int
remotePostTrigger(char *event, char *what, char **triggers)
{
	int	fd1, i, rc = 0;

	trigger_env("BKD", event, what);
	/*
	 * Process post trigger for remote client
	 *
	 * Post trigger output is not sent to remote client
	 * So redirect output to stderr
	 */
	fflush(stdout);
	fd1 = dup(1); assert(fd1 > 0);
	if (dup2(2, 1) < 0) perror("trigger: dup2");
	EACH(triggers) {
		unless (executable(triggers[i])) continue;
		rc = runit(triggers[i], what, 0);
		if (rc) break;
	}
	dup2(fd1, 1); close(fd1);
	return (rc);
}
