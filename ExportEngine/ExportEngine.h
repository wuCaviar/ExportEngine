#pragma once

// ExportEngine — EE_API export macro

#ifdef _MSC_VER
// C4251: STL members don't need DLL interface — safe with same compiler+CRT
#    pragma warning(disable : 4251)
#endif

#ifdef _WIN32
#    ifdef EXPORTENGINE_EXPORTS
#        define EE_API __declspec(dllexport)
#    else
#        define EE_API __declspec(dllimport)
#    endif
#else
#    define EE_API
#endif
