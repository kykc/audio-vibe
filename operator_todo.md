# Things I'll need to do

## Before public release

+* Audit the whole history tree for sensitive stuff (stray tokens, URLs, etc) before publish
+* Rename current README.md -> DEVELOPER.md
+* Author new README.md which will provide guidance for end-user
* Reconsider README.txt contents from the package
* Extend gitea workflow to automatically push to remote git repo and pre-release with zip?
* Pick a username for github publish (merge two if possible?)

## Postponed, but somewhat still relevant

* Verify that multiinstance works.
* Refine stats not to be so noisy (condence fast changing numbers to 23.4k? limit number of updates to n per second?)
* Why timeouts are not climbing after attaching to the idle device right after starting UI? (it's in a different protocol state, but I need to remind myself the difference)
* gracefully handle plugin path changes (plugin id? class?), concerns both main state and presets.
