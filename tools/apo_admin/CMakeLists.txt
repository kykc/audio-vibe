# apo_admin -- endpoint effect-chain administration, and the reversal contingency for everything
# the APO stage does to this machine. See src comments for why it is separate from regsvr32.

add_executable(apo_admin main.cpp)

target_link_libraries(apo_admin PRIVATE
    aip_common
    aip_protocol   # the two CLSIDs it recognises and switches between
    advapi32       # the registry, and the elevation check
    ole32          # initguid/mmdeviceapi pull it in
    shell32        # SHGetKnownFolderPath, for the %ProgramData% folder --register copies into
)

# Deliberately **not** manifested `requireAdministrator`, even though every mutation needs it.
#
# `--list` is a read-only registry walk and is the first thing anyone runs -- including to find
# out whether they need to do anything at all. Forcing the whole executable to elevate makes that
# fail outright from an ordinary shell, which is exactly the wrong answer to "what is installed?".
# The client reads the same registration unelevated for the same reason
# (`ipc/apo_registration.h`).
#
# Mutating paths check `elevated()` themselves and say so plainly, which is a better diagnostic
# than a UAC refusal.
