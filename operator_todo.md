# Things I'll need to do

# Client/VST host

* https://www.flaticon.com/free-icon/mixing-table_5903291?term=sound+mixer&related_id=5903291 how to make attribution properly
* Sizing of NAM plugin editor window is wrong on secondary ASUS monitor
* App starts where the mouse is. Maybe I want it to always start on primary (by default, no previous state)?
* EQ editor on the main monitor starts partially out-of-screen-bounds
* Do I need main window size/position save/restore? Probably yes.
* Do I need multiple tabs as previously? Probably no. But at least verify that multiinstance works.
* Filter device list to those that have APO registered. (others greyed out?)
* State management as such, text config. Preferrably yaml. Should be placed next to exe (portable). Or prioritized besides exe->appdata to support both use-cases.
* checkbox on chain item for bypass.
* Drag-and-drop reordering.
* Context menu for chain items.
* Refine stats not to be so noisy (condence fast changing numbers to 23.4k? limit number of updates to n per second?)
* Relax bus/channel requirements. If plugins want 32 channels and we only have 2 there's no need to decline -> we can provide 2 channels and 30 channels of silent throw-away buffers.
* plugins with no editor
* ability to save/load chain presets outside of the main state file
