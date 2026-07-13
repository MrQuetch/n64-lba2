# General sources
if(LBA2_TARGET_WIIU)
    # On Wii U the SDL3-using files are replaced by WIIU_BACKEND.CPP, which
    # provides equivalent symbols on top of WUT (OSScreen, ProcUI, OSGetSystemTime).
    # Excluded vs the SDL build:
    #   AVAILMEM, DISPOS, EVENTS, KEYBOARD, MOUSE, STRING, TIMER, WINDOW
    set(SOURCES_FILES CMDLINE.CPP DEFFILE.CPP EXIT.CPP
            FILECOPY.CPP FILENAME.CPP FILES.CPP HQFILE.CPP HQMEM.CPP HQRLOAD.CPP HQRMEM.CPP HQRMLOAD.CPP HQRRESS.CPP
            INPUT.CPP ITOA.CPP LOADMALL.CPP LOADSAVE.CPP N_MALLOC.CPP
            S_MALLOC.CPP SYSTEM.CPP UTILS.CPP
            WIIU_BACKEND.CPP)
    # LOGPRINT.CPP excluded: WIIU_BACKEND.CPP provides LogPrintf/LogPuts that
    # route directly to the OSScreen status display, so engine-side logging
    # becomes visible on the GamePad without a UART cable.
else()
    set(SOURCES_FILES AVAILMEM.CPP CMDLINE.CPP DEFFILE.CPP DISPOS.CPP EVENTS.CPP EXIT.CPP
            FILECOPY.CPP FILENAME.CPP FILES.CPP HQFILE.CPP HQMEM.CPP HQRLOAD.CPP HQRMEM.CPP HQRMLOAD.CPP HQRRESS.CPP
            INPUT.CPP ITOA.CPP KEYBOARD.CPP LOADMALL.CPP LOADSAVE.CPP LOGPRINT.CPP MOUSE.CPP N_MALLOC.CPP
            S_MALLOC.CPP STRING.CPP SYSTEM.CPP TIMER.CPP WINDOW.CPP UTILS.CPP)
endif()

# set(SOURCES_FILES_ASM CPU.ASM FASTCPYF.ASM LZ.ASM MOUSEDAT.ASM)
set(SOURCES_FILES_ASM CPU.CPP FASTCPYF.CPP LZ.CPP MOUSEDAT.CPP)

add_library(sys ${SOURCES_FILES} ${SOURCES_FILES_ASM})
target_link_libraries(sys svg)

# Configure all ASM files to be recognized as such (workaround for bug where .ASM uppercase extension is not recognized)
# foreach (FILE_ASM IN LISTS SOURCES_FILES_ASM)
#     set_source_files_properties(${FILE_ASM} PROPERTIES LANGUAGE ASM_MASM)
# endforeach ()
