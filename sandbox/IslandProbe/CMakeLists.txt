# Проба «своё окно + остров-карточка» — см. main.cpp. Черновик: собирается,
# чтобы проверяться, в приложение не входит.
#
# Проекция cppwinrt и загрузчик WinAppSDK берутся по тем же кэш-переменным,
# что заводит wxl.winui; свой find_package нужен потому, что импортированные
# цели vcpkg видны только в каталоге, где их нашли, и ниже.
find_package(cppwinrt CONFIG REQUIRED)

add_executable(IslandProbe WIN32 main.cpp)

target_link_libraries(IslandProbe PRIVATE
    Microsoft::CppWinRT
    "${WXL_WINDOWSAPPSDK_FOUNDATION_DIR}/lib/native/x64/Microsoft.WindowsAppRuntime.Bootstrap.lib"
)

target_include_directories(IslandProbe PRIVATE
    "${WXL_CPPWINRT_PROJECTION_DIR}"
    "${WXL_WINDOWSAPPSDK_FOUNDATION_DIR}/include"
)

# Загрузчик ищет пакет платформы при старте и обязан лежать рядом — как у
# Reader.
add_custom_command(TARGET IslandProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WXL_WINDOWSAPPSDK_FOUNDATION_DIR}/runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll"
            "$<TARGET_FILE_DIR:IslandProbe>"
    COMMENT "Копирование загрузчика Windows App Runtime рядом с IslandProbe")
