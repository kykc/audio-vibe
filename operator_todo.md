# Things I'll need to do

# Client/VST host

* https://www.flaticon.com/free-icon/mixing-table_5903291?term=sound+mixer&related_id=5903291 how to make attribution properly
* Verify that multiinstance works.
* Filter device list to those that have APO registered. (others greyed out?)
* Context menu for chain items (debatable)
* Refine stats not to be so noisy (condence fast changing numbers to 23.4k? limit number of updates to n per second?)
* Why timeouts are not climbing after attaching to the idle device right after starting UI?
* branding: name, proper small icon, etc.
* gracefully handle plugin path changes (plugin id? class?), concerns both main state and presets.
* if control is pressed when clicking on add -> add from file
* if control is pressed when clicking on editor -> built in editor even if there is non-default
