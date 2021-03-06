/*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* https://github.com/Roffild/RoffildLibrary
*/
#include "stdafx.h"
#include "private.h"
#include "resource.h"

HMODULE ghModule;
SRWLOCK __pyinit_lock;
BOOL APIENTRY DllMain(HMODULE hModule,
   DWORD  ul_reason_for_call,
   LPVOID lpReserved
)
{
   switch (ul_reason_for_call)
   {
   case DLL_PROCESS_ATTACH:
      __interps_count = 1;
      __interps = (stInterpreter**)realloc(__interps, 2 * sizeof(stInterpreter*));
      __interps[0] = (stInterpreter*)malloc(sizeof(stInterpreter));
      __interps[0]->id = 0;
      __interps[0]->interp = NULL;
      __clearInterp(__interps[0]);
      ghModule = hModule;
      InitializeSRWLock(&__interps_lock);
      InitializeSRWLock(&__pyinit_lock);
#ifdef PYTHONDLL_SUBINTERPRETERS
   case DLL_THREAD_ATTACH:
#endif
      AcquireSRWLockExclusive(&__interps_lock);
      __interps = (stInterpreter**)realloc(__interps, (__interps_count + 1) * sizeof(stInterpreter*));
      __interps[__interps_count] = (stInterpreter*)malloc(sizeof(stInterpreter));
      __interps[__interps_count]->id = GetCurrentThreadId();
      __interps[__interps_count]->interp = NULL;
      __clearInterp(__interps[__interps_count]);
      __interps_count++;
      ReleaseSRWLockExclusive(&__interps_lock);
      break;
#ifdef PYTHONDLL_SUBINTERPRETERS
   case DLL_THREAD_DETACH:
      pyFinalize();
      break;
   case DLL_PROCESS_DETACH:
      for (size_t x = 1; x < __interps_count; x++) {
         _PY_THREAD_START_OR(__interps[x], continue);
         __clearInterp(__interp);
         PyErr_Clear();
         Py_EndInterpreter(__interp->interp);
         __interp->interp = NULL;
         __clearInterp(__interp);
         PY_THREAD_ANY_STOP;
      }
#else
   case DLL_THREAD_ATTACH:
      break;
   case DLL_THREAD_DETACH:
      break;
   case DLL_PROCESS_DETACH:
      pyFinalize();
#endif
      PY_THREAD_MAIN_START_OR(break);
      Py_Finalize();
      break;
   }
   return TRUE;
}

_DLLSTD(mqlbool) pyIsInitialized()
{
   if (Py_IsInitialized()) {
      stInterpreter *stinterp = __getInterp();
      return stinterp != NULL && stinterp->interp != NULL;
   }
   return false;
}

_DLLSTD(mqlbool) pyInitialize(const mqlstring paths_to_packages, const mqlstring paths_to_dlls,
   const mqlbool console)
{
   if (console && GetConsoleWindow() == NULL) {
      AllocConsole();
      freopen("CONIN$", "r", stdin);
      freopen("CONOUT$", "w", stdout);
      freopen("CONOUT$", "w", stderr);
   }
   if (Py_IsInitialized() == 0) {
      AcquireSRWLockExclusive(&__pyinit_lock);
      if (Py_IsInitialized() == 0) {
         Py_SetPath(paths_to_packages);
         Py_InitializeEx(0); //abort() on error
         __interps[0]->interp = PyThreadState_Get();
      }
      ReleaseSRWLockExclusive(&__pyinit_lock);
   }
   stInterpreter *stinterp = __getInterp();
   if (stinterp->interp == NULL) {
      PY_THREAD_MAIN_START_OR(return false);
#ifdef PYTHONDLL_SUBINTERPRETERS
      stinterp = __setInterp(Py_NewInterpreter());
#else
      stinterp = __setInterp(PyThreadState_Get());
#endif
      if (stinterp->interp != NULL) {
         stinterp->main = PyImport_AddModule("__main__");
         if (stinterp->main != NULL) {
            PyObject *global = PyModule_GetDict(stinterp->main);
            PyObject *eval = NULL;
            PyCompilerFlags local_flags = {0};
            HRSRC hResource = FindResource(ghModule, MAKEINTRESOURCE(IDR_PYTHON), L"Python");
            HGLOBAL hMemory = LoadResource(ghModule, hResource);

            PyObject *code = PyUnicode_FromStringAndSize(
               (const char*)LockResource(hMemory),
               SizeofResource(ghModule, hResource));
            if (code != NULL) {
               eval = PyRun_StringFlags(PyUnicode_AsUTF8(code), Py_file_input,
                  global, global, &local_flags);
               Py_XDECREF(eval);
               Py_DECREF(code);
            }
            code = PyUnicode_FromString("import sys, os; sys.path = r'");
            PyUnicode_AppendAndDel(&code, PyUnicode_FromWideChar(paths_to_packages, -1));
            PyUnicode_AppendAndDel(&code, PyUnicode_FromString(
               ";'.strip(' ;').split(';') + sys.path;"
               "os.environ['PATH'] = r'"
            ));
            PyUnicode_AppendAndDel(&code, PyUnicode_FromWideChar(paths_to_dlls, -1));
            PyUnicode_AppendAndDel(&code, PyUnicode_FromString(
               ";'.strip(' ;') + ';' + os.environ['PATH']"
            ));
            if (code != NULL) {
               eval = PyRun_StringFlags(PyUnicode_AsUTF8(code), Py_file_input,
                  global, global, &local_flags);
               Py_XDECREF(eval);
               Py_DECREF(code);
            }
            FreeResource(hMemory);
            PyObject *exc_type, *exc_value, *exc_traceback;
            PyErr_Fetch(&exc_type, &exc_value, &exc_traceback);
            __overrideInterp(stinterp);
            PyErr_Restore(exc_type, exc_value, exc_traceback);
         }
      }
      PY_THREAD_MAIN_STOP;
   }
   return stinterp->interp != NULL;
}

_DLLSTD(void) pyFinalize()
{
   // https://github.com/numpy/numpy/issues/8097
   // https://bugs.python.org/issue34309
   PY_THREAD_START_OR(return);
   __clearInterp(__interp);
   PyErr_Clear();
#ifdef PYTHONDLL_SUBINTERPRETERS
   Py_EndInterpreter(__interp->interp);
#endif
   __interp->interp = NULL;
   __clearInterp(__interp);
   PY_THREAD_ANY_STOP;
}

_DLLSTD(mqlbool) pyEval(const mqlstring pycode, const mqlbool override_class)
{
   mqlbool ret = false;
   PY_THREAD_START_OR(return ret);
   PyErr_Clear();
   PyObject *code = PyUnicode_FromWideChar(pycode, -1);
   if (code != NULL) {
      PyObject *global = PyModule_GetDict(__interp->main);
      PyObject *eval = NULL;
      PyCompilerFlags local_flags = {0};
      eval = PyRun_StringFlags(PyUnicode_AsUTF8(code), Py_file_input,
         global, global, &local_flags);
      if (eval != NULL) {
         if (override_class) {
            __overrideInterp(__interp);
         }
         ret = true;
         Py_DECREF(eval);
      }
      Py_DECREF(code);
   }
   PY_THREAD_STOP;
   return ret;
}

_DLLSTD(mqlbool) pyIsError(const mqlbool clear)
{
   PY_THREAD_START_OR(return true);
   const mqlbool ret = PyErr_Occurred() != NULL;
   if (ret && clear) {
      PyErr_Clear();
   }
   PY_THREAD_STOP;
   return ret;
}

_DLLSTD(mqlint) pyGetErrorText(mqlstring _DLLOUTSTRING(buffer), const mqlint stringBufferLen)
{
   mqlint ret = -1;
   if (stringBufferLen < 1) {
      return ret;
   }
   PY_THREAD_START_OR(
      wchar_t str[] = L"The pyInitialize() was not called or returned an error.";
      Py_ssize_t size = wcslen(str);
      ret = (mqlint)size;
      if (size > stringBufferLen) {
         size = stringBufferLen;
      }
      wmemcpy(buffer, str, size);
      buffer[size] = 0;
      return ret;
   );
   if (PyErr_Occurred() == NULL) {
      ret = 0;
   }
   else {
      PyObject *exc_type, *exc_value, *exc_traceback;
      PyObject *result;
      Py_ssize_t size;
      mqlstring str;
      PyErr_Fetch(&exc_type, &exc_value, &exc_traceback);
      result = PyObject_CallObject(__interp->mql_stderr_truncate, NULL);
      Py_XDECREF(result);
      result = PySys_GetObject("stderr");
      PySys_SetObject("stderr", __interp->mql_stderr);
      PyErr_Restore(exc_type, exc_value, exc_traceback);
      PyErr_Print();
      PySys_SetObject("stderr", result);
      result = PyObject_CallObject(__interp->mql_stderr_getvalue, NULL);
      if (result != NULL) {
         str = PyUnicode_AsUnicodeAndSize(result, &size);
         ret = (mqlint)size;
         if (size > stringBufferLen) {
            size = stringBufferLen;
         }
         wmemcpy(buffer, str, size);
         buffer[size] = 0;
         Py_DECREF(result);
      }
   }
   PY_THREAD_STOP;
   return ret;
}
