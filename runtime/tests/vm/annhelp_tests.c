/*******************************************************************************
 * Copyright (c) 2021, 2021 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "vmaccess.h"

/**
 * 
 */
jboolean JNICALL
Java_org_openj9_test_annotation_ContainsRuntimeAnnotationTest_fieldContainsRuntimeAnnotation(JNIEnv *env, jclass jlClass, jobject jlrField, jstring annotationNameString)
{
	jboolean result = FALSE;
	j9object_t fieldObject = NULL;
	J9VMThread *vmThread = (J9VMThread *) env;
	J9JavaVM *vm = vmThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;
	char fieldAnnotationNameStackBuffer[J9VM_PACKAGE_NAME_BUFFER_LENGTH] = {0};
	J9UTF8 *annotationNameUTF8 = NULL;
	PORT_ACCESS_FROM_JAVAVM(vm);

	if (NULL == annotationNameString) {
		vmFuncs->setCurrentExceptionUTF(vmThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, "annotation name is null");
	} else {
		vmFuncs->internalEnterVMFromJNI(vmThread);
		fieldObject = J9_JNI_UNWRAP_REFERENCE(jlrField);

		if (NULL != fieldObject) {
			j9object_t annotationNameObj = J9_JNI_UNWRAP_REFERENCE(annotationNameString);

			annotationNameUTF8 = vmFuncs->copyStringToJ9UTF8WithMemAlloc(
				vmThread,
				annotationNameObj,
				J9_STR_NULL_TERMINATE_RESULT,
				"",
				0,
				fieldAnnotationNameStackBuffer,
				0
			);

			if (NULL == annotationNameUTF8) {
				vmFuncs->setNativeOutOfMemoryError(vmThread, 0, 0);
			} else {
				J9JNIFieldID *fieldID = vmThread->javaVM->reflectFunctions.idFromFieldObject(vmThread, NULL, fieldObject);
				J9Class *clazz = J9VM_J9CLASS_FROM_HEAPCLASS(vmThread, J9_JNI_UNWRAP_REFERENCE(jlClass));
				J9ROMFieldShape *fieldShape = fieldID->field;
				IDATA cpIndex = getConstantPoolIndexForField(clazz->romClass, fieldShape);

				if (cpIndex < 0) {
					vmFuncs->setCurrentExceptionUTF(vmThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, "field cannot be found");
				} else {
					result = (jboolean) fieldContainsRuntimeAnnotation(clazz, (UDATA) cpIndex, annotationNameUTF8);
				}

				if ((J9UTF8 *) fieldAnnotationNameStackBuffer != annotationNameUTF8) {
					j9mem_free_memory(annotationNameUTF8);
				}
			}
		}

		vmFuncs->internalExitVMToJNI(vmThread);
	}

	return result;
}

/**
 * 
 */
// jboolean JNICALL
// Java_org_openj9_test_annotation_ContainsRuntimeAnnotationTest_methodContainsRuntimeAnnotation(JNIEnv *env, jclass clazz, jstring childNameString, jstring parentNameString, jobject classLoaderObject)
// {

// }
