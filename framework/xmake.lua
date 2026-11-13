-- Cyberware.dll - F4SE plugin. Build root = this folder (framework/).
-- lib/commonlibf4 is NOT in git. Check out Dear-Modding-FO4/commonlibf4 @ a08c731eead4dfc6ea55aea4649c22aec05a47fe (the commit
-- PrismaUI F4 pins) into lib/commonlibf4, and Dear-Modding-FO4/commonlib-shared @ ed737c04811bf7c998f5a80340fd4ba5f9d3546b into
-- lib/commonlibf4/lib/commonlib-shared. Then: xmake f -m release -y && xmake -y
set_xmakever("3.0.0")
set_project("Cyberware")
set_version("0.1.0")
set_languages("c++23")

add_rules("mode.release", "mode.releasedbg", "mode.debug")
set_defaultmode("release")

-- Every build writes a PDB next to its DLL, so a crash in the DEPLOYED binary can be symbolized. A PDB from another build of the
-- same source does not match (the builds are not byte-reproducible), and a lane PDB misled the first Sandevistan crash read.
-- Set here at the root, so it also reaches commonlibf4 and commonlib-shared. /Zi only adds debug info; the code is still built
-- /O2. /DEBUG turns /OPT:REF and /OPT:ICF off by default, so they are set again on the DLL below (tools/pdb-info.ps1 checks the pair).
set_symbols("debug")

includes("lib/commonlibf4")

target("Cyberware")
    set_kind("shared")
    set_filename("Cyberware.dll")
    add_rules("commonlibf4.plugin", { name = "Cyberware", author = "Squeetsquib", version = "0.1.0" })
    add_includedirs("plugin", "core")
    add_files("plugin/*.cpp", "core/*.cpp")
    set_pcxxheader("plugin/PCH.h")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")
    add_cxflags("/permissive-", "/wd4200", "/wd4201", "/wd4324", "/utf-8")
    add_syslinks("Version", "Ole32", "OleAut32", "User32", "Shell32")
    add_shflags("/DEBUG:FULL", "/OPT:REF", "/OPT:ICF", { force = true })

-- Test harness plugin (cyberware/harness): reports game state for the capture tools and answers known nuisance modals.
-- Staged ONLY into the MO2 mod "Cyberware - Test Config", never with the player's build. docs/integration/TEST-HARNESS.md
target("CWHarness")
    set_kind("shared")
    set_filename("CWHarness.dll")
    add_rules("commonlibf4.plugin", { name = "CWHarness", author = "Squeetsquib", version = "0.1.0" })
    add_includedirs("plugin")
    add_files("../harness/*.cpp")
    set_pcxxheader("plugin/PCH.h")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")
    add_cxflags("/permissive-", "/wd4200", "/wd4201", "/wd4324", "/utf-8")
    add_syslinks("User32")

-- Same core, as a console exe: the headless harness on Windows (proves the MSVC build of the core).
target("cwtest")
    set_kind("binary")
    set_languages("c++20")
    add_files("core/*.cpp", "test/cwtest.cpp")
    add_cxflags("/utf-8", "/EHsc")
