#include "sdkconfig.h"

#ifdef CONFIG_IDF_TARGET_LINUX
#include "main_linux.cpp"
#else
#include "main_esp32.cpp"
#endif
