audio-ipc2 -- the APO, and the tools that manage it.

Everything here except `apo_admin --list` needs an ELEVATED command prompt. Registering the APO
and putting it into an endpoint's effect chain changes the machine, system-wide, for every
application that plays audio -- and until it is undone.

The two .exe files here need the Microsoft Visual C++ 2015-2022 x64 redistributable, which is a
machine prerequisite for this package and is not carried in it. Almost every Windows machine
already has it; a machine that does not says so plainly, with a dialog naming VCRUNTIME140.dll.
aip_apo.dll itself needs nothing -- it is built with a static runtime on purpose, because a DLL
loaded into the Windows audio engine cannot depend on a redistributable being present.

WHAT IS IN HERE

  aip_apo.dll     the APO. Loaded into audiodg.exe by the Windows audio engine, and hands every
                  block to aip_ui.exe in the folder above over shared memory.
  apo_admin.exe   what is in each endpoint's effect chain, and how to change it. Backs up what
                  it replaces, and can put it back.
  apo_host.exe    drives aip_apo.dll directly, with audiodg.exe out of the loop entirely, with a
                  bank of test signals. Nothing about the machine's audio is touched.

REGISTERING IT

  1. Put this folder where it is going to stay -- see MOVING IT below -- and open an elevated
     prompt in it.
  2. regsvr32 aip_apo.dll                 makes the class loadable. Touches no endpoint.
  3. apo_admin --list                     check: the GFX slot should still be whatever it was.
  4. apo_admin --install --restart-audio  puts the APO into the GFX slot of every render
                                          endpoint, saving what was there, and restarts the audio
                                          service so the change takes effect now rather than
                                          whenever the endpoint is next initialised.
  5. Play something. Then run aip_ui.exe from the folder above and press Attach.

UNDOING IT, in this order

  apo_admin --uninstall --restart-audio   restores exactly what the slot held before, including
                                          "nothing".
  regsvr32 /u aip_apo.dll                 unregisters the class and nothing else.

MOVING IT

Registration records THIS path: regsvr32 writes it into the registry and the audio engine loads
the DLL from there ever after. Move or rename this folder, or let the drive letter change, and
the engine looks for a DLL that is not there -- no APO runs, and nothing anywhere says why. Run
regsvr32 again from the new location, or unregister before moving.

WHERE IT CAN LIVE

The audio engine runs as a service account, not as you, so it has to be able to read this folder.
A folder under C:\Users\<you> -- Desktop, Downloads, Documents -- does not grant that, and the APO
will be registered, slotted, and never loaded. `icacls .` here should list BUILTIN\Users with
(RX); somewhere like C:\aip or a folder under C:\Program Files does.

IF NOTHING HAPPENS

  apo_admin --list                  shows every endpoint, what is in its GFX slot, and whether a
                                    modern slot (5, 6, 7) is populated -- a populated modern slot
                                    makes a correct GFX entry dead letter, and --install clears
                                    them for exactly that reason.
  apo_host --list-signals           proves the DLL loads and processes at all, with the audio
  apo_host --signal sine:1000:-12   engine out of the picture. Attach aip_ui.exe to it -- it is a
                                    protocol v1 king like any other.
