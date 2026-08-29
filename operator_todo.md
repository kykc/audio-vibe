# Things I'll need to do

# Client/VST host

* https://www.flaticon.com/free-icon/mixing-table_5903291?term=sound+mixer&related_id=5903291 how to make attribution properly
  -- credited in Help -> About now. Flaticon's own terms want the *author's* name in the credit
  and the icon page does not say who it is offline; check the page and add the name.
* Verify that multiinstance works.
* Filter device list to those that have APO registered. (others greyed out?)
* Context menu for chain items (debatable)
* Refine stats not to be so noisy (condence fast changing numbers to 23.4k? limit number of updates to n per second?)
* Why timeouts are not climbing after attaching to the idle device right after starting UI?
* branding: name, proper small icon, etc. -- name is VibeAudio now (About box, the settings folder,
  saved-file headers). The icon is still the flaticon one.
* gracefully handle plugin path changes (plugin id? class?), concerns both main state and presets.
