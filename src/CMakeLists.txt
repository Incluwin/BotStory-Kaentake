# Build injector
add_library(injector SHARED
    injector.cpp
    system.cpp
    bypass.cpp
    inlink.cpp
    resman.cpp
    wzmaplimit.cpp
    avatar.cpp
    itemeff.cpp
    resolution.cpp
    client_prefs.cpp
    mobhptag.cpp
    tooltip.cpp
    fusionanvil.cpp
    itemicon.cpp
    tempstat.cpp
    stringpool.cpp
    debug.cpp
    hook.cpp
    features.cpp
    quicklogin.cpp
    rememberpw.cpp
    cosmicSso.cpp
    addiesKaentakeCompile.cpp
    BossHP.cpp
    chronicle_extras.cpp
    enlargequickslot.cpp
    v83_version_guard.cpp
    shoulderslots.cpp
    iscorrectbodypart.cpp
    getbodypartfromitem.cpp
    ext_chainlightning.cpp
    getmobinfobyid.cpp
    getnpcbyid.cpp
    worldmapinfo.cpp
    questcolor.cpp
    serverpackets.cpp
    featurehandshake.cpp
    crewwnd.cpp
    statallap.cpp
    beginnerap.cpp
)

target_precompile_headers(injector PRIVATE pch.h)

target_compile_definitions(injector PRIVATE KAENTAKE_TARGET_v83)

target_link_libraries(injector PRIVATE
    Detours
    WzLib
    winhttp
)

set_target_properties(injector PROPERTIES
    OUTPUT_NAME ${PROJECT_NAME}
)

add_custom_command(TARGET injector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Custom.wz"
        "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
)


# Build launcher
add_executable(launcher WIN32
    launcher.manifest
    launcher.rc
    launcher.cpp
    debug.cpp
    patcher.cpp
    featurespage.cpp
)

target_link_options(launcher PRIVATE "/MANIFEST:NO")

target_link_libraries(launcher PRIVATE
    Detours
    winhttp
    bcrypt
)

set_target_properties(launcher PROPERTIES
    OUTPUT_NAME ${PROJECT_NAME}
)