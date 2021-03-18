/*******************************************************************************
 * Copyright (c) 2015, 2021 IBM Corp. and others
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

#include "cfreader.h"
#include "j9protos.h"

static I_32 skipAnnotationElement(J9ROMConstantPoolItem const * constantPool, U_8 const *data, U_8 const **pIndex, U_8 const * dataEnd);
static I_32 getAnnotationByType(J9ROMConstantPoolItem const *constantPool, J9UTF8 const *searchString, U_32 const numAnnotations, U_8 const *data, U_8 const **pIndex,  U_8 const *dataEnd);
static J9ROMFieldShape * getFieldShapeFromFieldRef(J9ROMClass *romClass, J9ROMFieldRef *romFieldRef);

/**
 * Walk the annotation to find the specified annotation provided as the searchString.
 *
 * @param (in) constantPool pointer to ROM class constant pool
 * @param (in) searchString name of the annotation class
 * @param (in) numAnnotations number of annotations
 * @param (in) data pointer to start of an attribute.
 * @param (inout) pIndex pointer to pointer to the current position in the attribute
 * @param (in) dataEnd pointer to the end of the data.
 * @return number of element/value pairs, -1 if searchString not found, -2 if an error occurred
 */
static I_32
getAnnotationByType(J9ROMConstantPoolItem const *constantPool, J9UTF8 const *searchString, U_32 const numAnnotations, U_8 const *data, U_8 const **pIndex, U_8 const *dataEnd)
{
	I_32 result = -1;
	U_32 errorCode = 0; /* used by CHECK_EOF */
	U_32 offset = 0; /* used by CHECK_EOF */
	U_8 const *index = *pIndex; /* used by CHECK_EOF */
	U_32 annCount = 0;

	for (annCount = 0; (annCount < numAnnotations) && (-1 == result); ++annCount) {
		J9UTF8 *className = NULL;
		U_32 numElementValuePairs = 0;
		U_16 annTypeIndex = 0;

		CHECK_EOF(2);
		NEXT_U16(annTypeIndex, index); /* annotation type_index */
		className = J9ROMCLASSREF_NAME((J9ROMClassRef*)constantPool+annTypeIndex);
		CHECK_EOF(2);
		NEXT_U16(numElementValuePairs, index); /* annotation num_element_value_pairs */

		if (J9UTF8_EQUALS(className, searchString)) {
			result = numElementValuePairs;
		} else {
			while (numElementValuePairs > 0) {
				data += 2; /* skip element_name_index */

				if (0 != skipAnnotationElement(constantPool, data, &index, dataEnd)) {
					result = -2; /* bad annotation */
					break;
				}
				numElementValuePairs--;
			}
		}
	}
	*pIndex = index;
	return result;
_errorFound:
	return -2;
}

/**
 * Skip past annotation elements.
 *
 * @param constantPool Pointer to the constant pool item.
 * @param data Pointer to annotation data.
 * @param pIndex Pointer to index (pointer) in the annotation data.
 * @param dataEnd Pointer to the end of the annotation data.
 * @return 0 if successful, -1 if an error occurs while skipping annotation elements.
 */
static I_32
skipAnnotationElement(J9ROMConstantPoolItem const *constantPool, U_8 const *data, U_8 const **pIndex, U_8 const *dataEnd)
{
	U_8 tag = 0;
	I_32 result = 0;
	U_32 errorCode = 0; /* used by CHECK_EOF */
	U_32 offset = 0; /* used by CHECK_EOF */
	U_8 const *index = *pIndex; /* used by CHECK_EOF */

	CHECK_EOF(1);
	NEXT_U8(tag, index); /* tag */

	switch (tag) {
		case 'B':
		case 'C':
		case 'D':
		case 'F':
		case 'I':
		case 'J':
		case 'S':
		case 'Z':
		case 's':
		case 'c':
			CHECK_EOF(2);
			data += 2; /* skip const_value_index or class_info_index (depending on tag) */
			break;
		case 'e':
			CHECK_EOF(4);
			data += 4; /* skip type_name_index and const_name_index */
			break;
		case '@': {
			U_32 numElementValuePairs = 0;
			U_32 j = 0;
			CHECK_EOF(4);
			data += 2; /* skip type_index */
			NEXT_U16(numElementValuePairs, index); /* num_element_value_pairs */
			/* skip each element in the annotation structure */
			for (j = 0; (j < numElementValuePairs) && (0 == result); j++) {
				result = skipAnnotationElement(constantPool, data, &index, dataEnd);
			}
			break;
		}
		/* structure: array_value contains num_values and values[num_values] */
		case '[': {
			U_32 numValues = 0;
			U_32 j = 0;
			CHECK_EOF(2);
			NEXT_U16(numValues, index); /* num_values */
			/* skip each element_value structure */
			for (j = 0; (j < numValues) && (0 == result); j++) {
				result = skipAnnotationElement(constantPool, data, &index, dataEnd);
			}
			break;
		}

		default:
			result = -1;
			break;
	}
	*pIndex = index;
	return result;

_errorFound:
	*pIndex = index;
	return -1;
}

/**
 * Check if a field contains the specified Runtime Visible annotation.
 *
 * @param clazz The class the field belongs to.
 * @param cpIndex The constant pool index of the field.
 * @param annotationName The name of the annotation to check for.
 * @return TRUE if the annotation is found, FALSE otherwise.
 */
BOOLEAN
fieldContainsRuntimeAnnotation(J9Class *clazz, UDATA cpIndex, J9UTF8 *annotationName)
{
	BOOLEAN annotationFound = FALSE;
	J9ROMClass *romClass = clazz->romClass;
	J9ROMConstantPoolItem *constantPool = J9_ROM_CP_FROM_ROM_CLASS(clazz->romClass);
	J9ROMFieldRef *romFieldRef = (J9ROMFieldRef *) &constantPool[cpIndex];
	J9ROMFieldShape *romFieldShape = getFieldShapeFromFieldRef(romClass, romFieldRef);

	if (NULL != romFieldShape) {
		U_32 *fieldAnnotationData = getFieldAnnotationsDataFromROMField(romFieldShape);
		U_8 *data = (U_8 *) (fieldAnnotationData + 1);

		annotationFound = findRuntimeVisibleAnnotation(data, annotationName, constantPool);
	}

	return annotationFound;
}

/**
 * Find the corresponding J9ROMFieldShape for a given J9ROMFieldRef.
 *
 * @param romClass The ROM Class the field belongs to.
 * @param romFieldRef The Field Ref to find the matching Field Shape for.
 * @return A pointer to the corresponding Field Shape or NULL if not found.
 */
static J9ROMFieldShape *
getFieldShapeFromFieldRef(J9ROMClass *romClass, J9ROMFieldRef *romFieldRef)
{
	J9ROMFieldShape *romFieldShape = NULL;
	J9ROMNameAndSignature *fieldRefNameAndSig = J9ROMFIELDREF_NAMEANDSIGNATURE(romFieldRef);
	J9UTF8 *fieldRefName = J9ROMNAMEANDSIGNATURE_NAME(fieldRefNameAndSig);
	J9UTF8 *fieldRefSignature = J9ROMNAMEANDSIGNATURE_SIGNATURE(fieldRefNameAndSig);
	J9ROMFieldWalkState state;
	J9ROMFieldShape *romFieldPtr = romFieldsStartDo(romClass, &state);
	U_32 i;

	for (i = 0; (i < romClass->romFieldCount) && (NULL != romFieldPtr); i++) {
		J9UTF8 *fieldShapeName = J9ROMFIELDSHAPE_NAME(romFieldPtr);
		J9UTF8 *fieldshapeSignature = J9ROMFIELDSHAPE_SIGNATURE(romFieldPtr);

		if (J9UTF8_EQUALS(fieldRefName, fieldShapeName) && J9UTF8_EQUALS(fieldRefSignature, fieldshapeSignature)) {
			romFieldShape = romFieldPtr;
			break;
		}

		romFieldPtr = romFieldsNextDo(&state);
	}

	return romFieldShape;
}

/**
 * Get the corresponding constant pool index for a given J9ROMFieldShape.
 *
 * @param romClass The ROM Class the field belongs to.
 * @param romFieldShape The field to grab the constant pool index for.
 * @return The constant pool index for the field, or -1 if the index is not found.
 */
IDATA
getConstantPoolIndexForField(J9ROMClass *romClass, J9ROMFieldShape *romFieldShape)
{
	U_16 cpCount = romClass->romConstantPoolCount;
	J9ROMConstantPoolItem *constantPool = J9_ROM_CP_FROM_ROM_CLASS(romClass);
	U_32 *cpShapeDescription = J9ROMCLASS_CPSHAPEDESCRIPTION(romClass);
	BOOLEAN cpIndexFound = FALSE;
	IDATA cpIndex;

	for (cpIndex = 1; cpIndex < cpCount; cpIndex++) {
		J9ROMConstantPoolItem *cpItem = constantPool + cpIndex;
		U_32 cpType = J9_CP_TYPE(cpShapeDescription, cpIndex);

		if (J9CPTYPE_FIELD == cpType) {
			J9ROMFieldShape *fieldShape = getFieldShapeFromFieldRef(romClass, (J9ROMFieldRef *) cpItem);

			if (romFieldShape == fieldShape) {
				cpIndexFound = TRUE;
				break;
			}
		}
	}

	if (!cpIndexFound) {
		cpIndex = -1;
	}

	return cpIndex;
}

/**
 * Check if a method contains the specified Runtime Visible annotation.
 *
 * @param clazz The class the method belongs to.
 * @param cpIndex The constant pool index of the method.
 * @param annotationName The name of the annotation to check for.
 * @return TRUE if the annotation is found, FALSE otherwise.
 */
BOOLEAN
methodContainsRuntimeAnnotation(J9Class *clazz, UDATA cpIndex, J9UTF8 *annotationName)
{
	BOOLEAN annotationFound = FALSE;
	J9ROMClass *romClass = clazz->romClass;
	J9ROMConstantPoolItem *constantPool = J9_ROM_CP_FROM_ROM_CLASS(clazz->romClass);
	J9ROMMethodRef *romMethodRef = (J9ROMMethodRef *) &constantPool[cpIndex];
	J9ROMMethod *romMethod = getMethodFromMethodRef(romClass, romMethodRef);

	if (NULL != romMethod) {
		U_32 *methodAnnotationData = getMethodAnnotationsDataFromROMMethod(romMethod);
		U_8 *data = (U_8 *) (methodAnnotationData + 1);

		annotationFound = findRuntimeVisibleAnnotation(data, annotationName, constantPool);
	}

	return annotationFound;
}

/**
 * Find the corresponding J9ROMMethod for a given J9ROMMethodRef.
 *
 * @param romClass The ROM Class the method belongs to.
 * @param romMethodRef The Method Ref to find the matching Method Shape for.
 * @return A pointer to the corresponding Method Shape or NULL if not found.
 */
static J9ROMMethod *
getMethodFromMethodRef(J9ROMClass *romClass, J9ROMMethodRef *romMethodRef)
{
	J9ROMMethod *romMethod = NULL;
	J9ROMNameAndSignature *methodRefNameAndSig = J9ROMMETHODREF_NAMEANDSIGNATURE(romMethodRef);
	J9UTF8 *methodRefName = J9ROMNAMEANDSIGNATURE_NAME(methodRefNameAndSig);
	J9UTF8 *methodRefSignature = J9ROMNAMEANDSIGNATURE_SIGNATURE(methodRefNameAndSig);
	J9ROMMethod *romMethodPtr = J9ROMCLASS_ROMMETHODS(romClass);
	U_32 i;

	for (i = 0; (i < romClass->romMethodCount) && (NULL != romMethodPtr); i++) {
		J9UTF8 *methodName = J9ROMFIELDSHAPE_NAME(romMethodPtr);
		J9UTF8 *methodSignature = J9ROMFIELDSHAPE_SIGNATURE(romMethodPtr);

		if (J9UTF8_EQUALS(methodRefName, methodName) && J9UTF8_EQUALS(methodRefSignature, methodSignature)) {
			romMethod = romMethodPtr;
			break;
		}

		romFieldPtr = nextROMMethod(romMethodPtr);
	}

	return romMethod;
}

/**
 * Check if the provided Runtime Visible annotation data contains the specified annotation.
 *
 * @param data The Runtime Visible annotation data.
 * @param annotationName The annotation to check for.
 * @param constantPool The constant pool where the data is located.
 * @return TRUE if the annotation is found, FALSE otherwise.
 */
static BOOLEAN
findRuntimeVisibleAnnotation(U_8 *data, J9UTF8 *annotationName, J9ROMConstantPoolItem *constantPool)
{
	U_8 *dataEnd = NULL;
	U_32 errorCode = 0; /* used by CHECK_EOF */
	U_32 offset = 0; /* used by CHECK_EOF */
	U_8 const *index = data; /* used by CHECK_EOF */
	U_32 attributeLength = 0;
	U_32 numAnnotations = 0;
	I_32 getAnnotationResult = -1;

	CHECK_EOF(2);
	data += 2; /* skip attribute_name_index */
	CHECK_EOF(4);
	NEXT_U32(attributeLength, index); /* attribute_length */
	dataEnd = data + attributeLength;
	CHECK_EOF(2);
	NEXT_U16(numAnnotations, index); /* num_annotations */

	getAnnotationResult = getAnnotationByType(constantPool, annotationName, numAnnotations, data, &index, dataEnd);

	if (getAnnotationResult > 0) {
		return TRUE;
	}

_errorFound:
	return FALSE;
}



// FROM VMHELPERS
// #define GETNEXT_U32(value, index) (value = *(U_32*)index, index += 4, value)
// #define GETNEXT_U16(value, index) (value = *(U_16*)index, index += 2, value)
// #define GETNEXT_U8(value, index) (value = *(index++))

	// /**
	//  * Determine if the field is stable (annotated with @Stable).
	//  *
	//  * If the field is stable and its static type is an array, then all the array
	//  * elements are considered to be stable as well.
	//  *
	//  * @param clazz the class that owns the field
	//  * @param cpindex the cpindex of the field to query
	//  * @return true if the field is stable, false otherwise
	//  */
	// static VMINLINE bool
	// isFieldStable(J9Class *clazz, UDATA cpIndex)
	// {
	// 	bool isStable = false;
	// 	// J9ConstantPool *constantPool = clazz->ramConstantPool->romConstantPool;
	// 	// J9ROMFieldRef *romFieldRef = (J9ROMFieldRef *) &constantPool[cpIndex];
	// 	// J9ROMFieldShape *romField = J9ROMFIELDREF_NAMEANDSIGNATURE(romFieldRef);
	// 	// U_32 *fieldAnnotationData = getFieldAnnotationsDataFromROMField(romField);

	// 	// J9ROMFieldRef VS J9ROMFieldShape????

	// 	J9ROMConstantPoolItem *constantPool = J9_ROM_CP_FROM_ROM_CLASS(clazz->romClass);
	// 	J9ROMFieldRef *romFieldRef = (J9ROMFieldRef *) &constantPool[cpIndex];
	// 	J9ROMNameAndSignature *fieldNameAndSig = J9ROMFIELDREF_NAMEANDSIGNATURE(romFieldRef);
	// 	J9ROMFieldShape* romField = J9ROMCLASS_ROMFIELDS(clazz->romClass);

	// 	J9ROMCLASS_ROMFIELDS(base) NNSRP_GET((base)->romFields, struct J9ROMFieldShape*)

	// 	typedef struct J9ROMConstantPoolItem {
	// 		U_32 slot1;
	// 		U_32 slot2;
	// 	} J9ROMConstantPoolItem;

	// 	typedef struct J9ROMFieldRef {
	// 		U_32 classRefCPIndex;
	// 		J9SRP nameAndSignature;
	// 	} J9ROMFieldRef;

	// 	typedef struct J9ROMFieldShape {
	// 		struct J9ROMNameAndSignature nameAndSignature;
	// 		U_32 modifiers;
	// 	} J9ROMFieldShape;

	// 	// take field ref name
	// 	// iterate through field shape names until you find the one that matches

	// 	// getFieldAnnotationsDataFromROMField needs J9ROMFieldShape
	// 	U_32 *fieldAnnotationData = getFieldAnnotationsDataFromROMField(romField);

	// 	if (NULL != fieldAnnotationData) {
	// 		isStable = containsRuntimeAnnotation(constantPool, fieldAnnotationData, "Stable");
	// 	}

	// 	// j9tty_printf( PORTLIB, "  Name: %i -> %s\n", field->nameIndex, classfile->constantPool[field->nameIndex].bytes);
	// 	// j9tty_printf( PORTLIB, "  Signature: %i -> %s\n", field->descriptorIndex, classfile->constantPool[field->descriptorIndex].bytes);
	// 	// j9tty_printf( PORTLIB, "  Access Flags: 0x%X ( ", field->accessFlags);
	// 	// printModifiers(PORTLIB, field->accessFlags, INCLUDE_INTERNAL_MODIFIERS, MODIFIERSOURCE_FIELD);
	// 	// j9tty_printf( PORTLIB, " )\n");
	// 	// j9tty_printf( PORTLIB, "  Attributes (%i):\n", field->attributesCount);
	// 	// for(i = 0; i < field->attributesCount; i++)
	// 	// {
	// 	// 	dumpAttribute(classfile, field->attributes[i], 2);
	// 	// }

	// 	return isStable;
	// }

	// /**
	//  * Determine if the method is tagged with ForceInline (annotated with @ForceInline).
	//  *
	//  * @param clazz the class that owns the field
	//  * @param cpindex the cpindex of the method to query
	//  * @return true if the method is tagged with ForceInline, false otherwise
	//  */
	// static VMINLINE bool
	// isMethodTaggedWithForceInline(J9Class *clazz, UDATA cpIndex)
	// {
	// 	bool forceInline = false;

	// 	J9ROMConstantPoolItem *constantPool = J9_ROM_CP_FROM_ROM_CLASS(clazz->romClass);
	// 	J9ROMMethodRef *romMethodRef = (J9ROMMethodRef *) &constantPool[cpIndex];
	// 	J9ROMNameAndSignature *fieldNameAndSig = J9ROMFIELDREF_NAMEANDSIGNATURE(romMethodRef);

	// 	// iterate methods in the class and match the name and sig to the methodref name and sig
	// 	// J9ROMMethod
	// 	U_32 *fieldAnnotationData = getMethodAnnotationsDataFromROMMethod(romField);

	// 	if (NULL != fieldAnnotationData) {
	// 		forceInline = containsRuntimeAnnotation(constantPool, fieldAnnotationData, "ForceInline");
	// 	}

	// 	return forceInline;
	// }

	// /**
	//  * Check if the annotation data contains the requested annotation.
	//  *
	//  * @param constantPool The constant pool, to grab info about annotation data indices.
	//  * @param annotationData Pointer to RuntimeVisibleAnnotations_attribute data.
	//  * @param requestedAnnotation The name of the annotation to check for.
	//  * @return true if the annotation data contains the requested annotation, false otherwise.
	//  */
	// static VMINLINE bool
	// containsRuntimeAnnotation(const J9ConstantPool *constantPool, const U_32 *annotationData, const U_8 *requestedAnnotation)
	// {
	// 	bool containsAnnotation = false;
	// 	U_32 annotationDataLength = annotationData[0];
	// 	U_8 *annotationDataPtr = (U_8 *) (annotationData + 1);
	// 	UDATA annotationDataIndex = 0;
	// 	U_16 numAnnotations = 0;
	// 	U_32 i = 0;

	// 	/* value: attribute_name_index is U_16 */
	// 	GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 	/* value: attribute_length is U_32 */
	// 	GETNEXT_U32(annotationDataIndex, annotationDataPtr);
	// 	/* value: num_annotations is U_16 */
	// 	numAnnotations = GETNEXT_U16(annotationDataIndex, annotationDataPtr);

	// 	/* array: annotations contains annotation structures */
	// 	for (i = 0; i < numAnnotations; i++) {
	// 		/* structure: annotation contains type_index, num_element_value_pairs and structure element_value_pairs */
	// 		/* value: type_index is U_16 */
	// 		U_16 annotationTypeIndex = GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 		/* structure: CONSTANT_Utf8_info contains tag, length and array bytes */
	// 		U_8 *annotationUtf8Info = &(constantPool[annotationTypeIndex]);
	// 		U_8 annotationUtf8InfoIndex = 0;
	// 		U_16 annotationNameLength = 0;
	// 		U_8 *annotationName = NULL;

	// 		/* value: tag is U_8 */
	// 		GETNEXT_U8(annotationUtf8InfoIndex, annotationUtf8Info);
	// 		/**
	// 		 * Exclude the leading type indicator and trailing semicolon of field descriptor
	// 		 * i.e. LClassName; --> ClassName
	// 		 */
	// 		/* value: length is U_16 */
	// 		annotationNameLength = GETNEXT_U16(annotationUtf8InfoIndex, annotationUtf8Info) - 2;
	// 		annotationName = annotationUtf8Info + 1;

	// 		if ((0 == memcmp(annotationName, requestedAnnotation, annotationNameLength)) {
	// 			containsAnnotation = true;
	// 			break;
	// 		}

	// 		walkAnnotation(annotationDataIndex, annotationDataPtr);

	// 		/* value: num_element_value_pairs is U_16 */
	// 		U_16 numElementValuePairs = GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 		U_32 i = 0;

	// 		/* array: element_value_pairs contains a U_16 and an element_value */
	// 		for (i = 0; i < numElementValuePairs; i++) {
	// 			/* value: element_name_index is U_16 */
	// 			GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 			/* walk element_value structure */
	// 			walkAnnotationElementValue(annotationDataIndex, annotationDataPtr);
	// 		}
	// 	}

	// 	return containsAnnotation;
	// }

	// /**
	//  * Walk through an annotation's element_value_pairs structure.
	//  *
 	//  * @param annotationDataIndex The index into the annotation data.
	//  * @param annotationDataPtr Pointer to the annotation data.
	//  */
	// static VMINLINE void walkAnnotation(U_32 annotationDataIndex, U_8 *annotationDataPtr)
	// {
	// 	/* value: num_element_value_pairs is U_16 */
	// 	U_16 numElementValuePairs = GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 	U_32 i = 0;

	// 	/* array: element_value_pairs contains a U_16 and an element_value */
	// 	for (i = 0; i < numElementValuePairs; i++) {
	// 		/* value: element_name_index is U_16 */
	// 		GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 		/* walk element_value structure */
	// 		walkAnnotationElementValue(annotationDataIndex, annotationDataPtr);
	// 	}
	// }

	// /**
	//  * Walk through an element_value structure.
	//  *
 	//  * @param annotationDataIndex The index into the annotation data.
	//  * @param annotationDataPtr Pointer to the annotation data.
	//  */
	// static VMINLINE void walkAnnotationElementValue(U_32 annotationDataIndex, U_8 *annotationDataPtr)
	// {
	// 	/* structure: element_value contains tag and a union of the types outlined in the Switch below */
	// 	/* value: tag is a U_8 */
	// 	U_8 elementValueTag = GETNEXT_U8(annotationDataIndex, annotationDataPtr);

	// 	/* Based on the element_value tag, determine the value of the element_value's union */
	// 	switch (elementValueTag) {
	// 		/* value: const_value_index is U_16 */
	// 		case 'B':
	// 		case 'C':
	// 		case 'D':
	// 		case 'F':
	// 		case 'I':
	// 		case 'J':
	// 		case 'S':
	// 		case 'Z':
	// 		case 's':
	// 		/* value: class_info_index is U_16 */
	// 		case 'c':
	// 			GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 			break;
	// 		/* structure: enum_const_value contains type_name_index and const_name_index */
	// 		case 'e': {
	// 			/* value: type_name_index is U_16 */
	// 			GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 			/* value: const_name_index is U_16 */
	// 			GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 			break;
	// 		}
	// 		/* value: annotation_value is an annotation type */
	// 		case '@': {
	// 			/* value: type_index is U_16 */
	// 			GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 			/* walk annotation structure */
	// 			walkAnnotation(annotationDataIndex, annotationDataPtr);
	// 			break;
	// 		}
	// 		/* structure: array_value contains num_values and values[num_values] */
	// 		case '[': {
	// 			/* value: num_values is U_16 */
	// 			U_16 numValues = GETNEXT_U16(annotationDataIndex, annotationDataPtr);
	// 			U_32 i = 0;
	// 			for (i = 0; i < numValues; i++) {
	// 				/* walk element_value structure */
	// 				walkAnnotationElementValue(annotationDataIndex, annotationDataPtr);
	// 			}
	// 			break;
	// 		}
	// 		default:
	// 			break;
	// 	}
	// }
