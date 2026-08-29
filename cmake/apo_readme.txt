VibeAudio -- the shell, the APO, and the tools that manage it.

If this arrived as a zip: unblock the archive before extracting it -- right-click package.zip,
Properties, Unblock. Windows marks a downloaded archive, Explorer copies that mark onto every file
it extracts, and you then get a warning per executable instead of one.

Running vibeaudio.exe costs nothing and is undone by closing it. Everything else here -- registering
the APO, and putting it into an endpoint's effect chain -- changes the machine, system-wide, for
every application that plays audio, until it is undone. The shell's File menu does both for you,
asking Windows for administrator rights for that one action. The command lines below are the same
two steps for anyone who would rather see them happen, and they want an ELEVATED prompt. The one
exception is `apo_admin --list`, which only reads.

The .exe files here need the Microsoft Visual C++ 2015-2022 x64 redistributable, which is a
machine prerequisite for this package and is not carried in it. Almost every Windows machine
already has it; a machine that does not says so plainly, with a dialog naming VCRUNTIME140.dll.
aip_apo.dll itself needs nothing -- it is built with a static runtime on purpose, because a DLL
loaded into the Windows audio engine cannot depend on a redistributable being present.

WHAT IS IN HERE

  vibeaudio.exe      the shell: the rack, the plugin browser, and the Attach button. Needs no
                  elevation and changes nothing outside this folder. Its File menu has both
                  installation steps below -- Register APO and Audio Device Settings -- each
                  asking Windows for administrator rights for that one action only.
  LICENSE         the MIT license this software is released under. Qt, whose DLLs are in this
                  folder, is used under the LGPLv3.
  aip_scan.exe    scans VST3 plugins out of process, so a plugin that crashes takes the scanner
                  with it and not the shell. Launched by vibeaudio.exe; not run by hand.
  aip_apo.dll     the APO. Loaded into audiodg.exe by the Windows audio engine, and hands every
                  block to vibeaudio.exe over shared memory.
  apo_admin.exe   what is in each endpoint's effect chain, and how to change it. Backs up what
                  it replaces, and can put it back.
  apo_host.exe    drives aip_apo.dll directly, with audiodg.exe out of the loop entirely, with a
                  bank of test signals. Nothing about the machine's audio is touched.

  vibeaudio.yaml, qt.conf and plugins/ belong to vibeaudio.exe. The .yaml being here is what makes
  this a portable install -- settings stay in this folder rather than in %APPDATA%.

REGISTERING IT, from the shell

  1. Run vibeaudio.exe. File -> Register APO copies aip_apo.dll into
     %ProgramData%\VibeAudio and makes the class loadable from there. Touches no endpoint.
  2. File -> Audio Device Settings, tick the device you want processed, Apply. That puts the APO
     into its GFX slot, saving what was there, and restarts the audio service.
  3. Play something. Then press Attach.

REGISTERING IT, by hand

  Same two steps, from an elevated prompt in this folder. Step 2 is what makes the folder want to
  stay put -- see MOVING IT.

  1. apo_admin --register                 copies the APO to %ProgramData%\VibeAudio and makes the
                                          class loadable from there. Touches no endpoint.
     regsvr32 aip_apo.dll                 the older way: registers the DLL WHERE IT IS, with no
                                          copy. Same effect, and this folder is then pinned.
  2. apo_admin --list                     check: the GFX slot should still be whatever it was.
  3. apo_admin --install --restart-audio  puts the APO into the GFX slot of every render
                                          endpoint, saving what was there, and restarts the audio
                                          service so the change takes effect now rather than
                                          whenever the endpoint is next initialised.

UNDOING IT, in this order

  apo_admin --uninstall --restart-audio   restores exactly what the slot held before, including
                                          "nothing". Or untick the devices in Audio Device
                                          Settings, which is the same thing.
  regsvr32 /u "%ProgramData%\VibeAudio\aip_apo.dll"
                                          unregisters the class and nothing else. Point it at the
                                          copy that was registered, which is the one --register
                                          made -- not necessarily the one in this folder.

MOVING IT

Registration records the path of the DLL it registered, and the audio engine loads it from there
ever after. Move or rename that folder, or let the drive letter change, and the engine looks for a
DLL that is not there -- no APO runs, and nothing anywhere says why.

Register APO (and `apo_admin --register`) is the answer to that: it registers a copy in
%ProgramData%\VibeAudio, which nobody moves, so THIS folder stays portable and can be copied
around freely afterwards. Registering with regsvr32 instead pins this folder, and moving it then
means running regsvr32 again from the new location, or unregistering before the move.

WHERE IT CAN LIVE

The audio engine runs as a service account, not as you, so it has to be able to read whatever
folder the registered DLL is in. A folder under C:\Users\<you> -- Desktop, Downloads, Documents --
does not grant that, and the APO will be registered, slotted, and never loaded. %ProgramData% does,
which is the other reason Register APO copies there rather than registering in place.

If you use regsvr32 instead, this folder is the one that has to be readable: `icacls .` here should
list BUILTIN\Users with (RX), and somewhere like C:\aip or a folder under C:\Program Files does.
vibeaudio.exe runs as you and does not care where it is; only the DLL the audio engine reads does.

IF NOTHING HAPPENS

  apo_admin --list                  shows each endpoint that is present -- default device first --
                                    what is in its GFX slot, and whether a modern slot (5, 6, 7)
                                    is populated. A populated modern slot makes a correct GFX
                                    entry dead letter, and --install clears them for exactly that
                                    reason.
  apo_admin --list --show-all       the same, plus the endpoints that are disabled, unplugged or
                                    gone. A slot written to one of those is still written, and is
                                    still there when the device comes back.
  apo_host --list-signals           proves the DLL loads and processes at all, with the audio
  apo_host --signal sine:1000:-12   engine out of the picture. Attach vibeaudio.exe to it -- it is a
                                    protocol v1 king like any other.
