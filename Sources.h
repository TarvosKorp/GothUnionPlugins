// Supported with union (c) 2018-2021 Union team
// Add your sources this

// Automatically generated block
#pragma region Includes
// Подключение кода плагина по конфигурации сборки.
// Каждая версия движка использует свой Plugin-файл (свои адреса хуков).
// Выбор по ENGINE (определяется в Interface.cpp): Engine_G1/G1A/G2/G2A.
#if ENGINE == Engine_G1
#include "Plugin_G1.cpp"
#elif ENGINE == Engine_G1A
#include "Plugin_G1A.cpp"
#elif ENGINE == Engine_G2
#include "Plugin_G2.cpp"
#elif ENGINE == Engine_G2A
#include "Plugin_G2A.cpp"
#else
#error "zMirrorWorld: движок не выбран (ENGINE)"
#endif
#pragma endregion

// ...