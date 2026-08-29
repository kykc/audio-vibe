# Things I'll need to do

# Client/VST host

* Verify that multiinstance works.
* Refine stats not to be so noisy (condence fast changing numbers to 23.4k? limit number of updates to n per second?)
* Why timeouts are not climbing after attaching to the idle device right after starting UI?
* gracefully handle plugin path changes (plugin id? class?), concerns both main state and presets.
