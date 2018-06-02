#include "EXP_PythonUtils.h"
#include "EXP_ListWrapper.h"

PyObject *EXP_ConvertToPython(EXP_ListWrapper *ptr)
{
	return ptr->NewProxy(true);
}

PyObject *EXP_ConvertToPython(EXP_Value *ptr)
{
	if (ptr) {
		 return ptr->GetProxy();
	}
	Py_RETURN_NONE;
}
