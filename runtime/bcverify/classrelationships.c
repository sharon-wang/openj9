/*******************************************************************************
 * Copyright (c) 2019, 2020 IBM Corp. and others
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

#include "j9protos.h"
#include "j9consts.h"
#include "cfreader.h"
#include "ut_j9bcverify.h"
#include "omrlinkedlist.h"

/* Class Relationship Snippet functions */
static void processClassRelationshipSnippetsNoCachedData(J9BytecodeVerificationData *verifyData, IDATA *reasonCode);
static void processClassRelationshipSnippetsUsingCachedData(J9BytecodeVerificationData *verifyData, uint8_t *snippetsDataDescriptorAddress, IDATA *reasonCode);
static void checkSnippetRelationship(J9BytecodeVerificationData *verifyData, U_8 *childClassName, UDATA childClassNameLength, U_8 *parentClassName, UDATA parentClassNameLength, IDATA *reasonCode);
static char *generateClassRelationshipSnippetsKey(J9JavaVM *vm, J9VMThread *vmThread, U_8 *className, UDATA classNameLength);
static IDATA allocateClassRelationshipSnippetsHashTable(J9BytecodeVerificationData *verifyData);
static void freeClassRelationshipSnippetsHashTable(J9BytecodeVerificationData *verifyData);
static UDATA relationshipSnippetHashFn(void *key, void *userData);
static UDATA relationshipSnippetHashEqualFn(void *leftKey, void *rightKey, void *userData);
static UDATA getTotalUTF8Size(J9BytecodeVerificationData *verifyData);
static IDATA storeToDataBuffer(J9BytecodeVerificationData *verifyData, uint8_t *dataBuffer, J9SharedClassRelationshipSnippet *dataBufferSnippetStart, J9UTF8 *dataBufferUTF8Start, UDATA snippetCount);
static UDATA setCurrentAndNextUTF8s(J9BytecodeVerificationData *verifyData, J9UTF8 **utf8Address, J9UTF8 **nextUTF8Address, UDATA classNameIndex);
static J9UTF8 *getUTF8Address(J9BytecodeVerificationData *verifyData, J9UTF8 **nextUTF8Address, UDATA classNameIndex);
static J9UTF8 *getUTF8AddressFromArray(J9BytecodeVerificationData *verifyData, J9UTF8 **nextUTF8Address, J9ClassRelationshipClassNameIndex **classNamesArray, UDATA totalNumberOfIndices, UDATA classNameIndex);
static J9UTF8 *getUTF8AddressFromHashTable(J9BytecodeVerificationData *verifyData, J9UTF8 **nextUTF8Address, J9HashTable *table, UDATA classNameIndex);
static J9HashTable *hashRelationshipClassNameTableNew(J9JavaVM *vm);
static void hashClassRelationshipClassNameTableFree(J9VMThread *vmThread, J9HashTable *relationshipClassNameHashTable);
static UDATA relationshipClassNameHashFn(void *key, void *userData);
static UDATA relationshipClassNameHashEqualFn(void *leftKey, void *rightKey, void *userData);

/* Class Relationship functions */
static UDATA allocateClassRelationshipTableAndPool(J9ClassLoader *classLoader, J9JavaVM *vm);
static J9ClassRelationshipNode *allocateClassRelationshipNode(J9VMThread *vmThread, J9ClassLoader *classLoader, U_8 *className, UDATA classNameLength);
static J9ClassRelationship *findClassRelationship(J9VMThread *vmThread, J9ClassLoader *classLoader, U_8 *className, UDATA classNameLength);
static void freeClassRelationshipNodes(J9VMThread *vmThread, J9ClassLoader *classLoader, J9ClassRelationship *relationship);
static UDATA relationshipHashFn(void *key, void *userData);
static UDATA relationshipHashEqualFn(void *leftKey, void *rightKey, void *userData);

/**
 * Class Relationship Snippet APIs (J9ClassRelationshipSnippet)
 */

/**
 * Record a class relationship snippet and save it locally in verifyData->classRelationshipSnippetsHashTable.
 *
 * Set reasonCode to BCV_ERR_INSUFFICIENT_MEMORY if record unsuccessful.
 *
 * Return TRUE if record is successful, FALSE otherwise.
 */
IDATA
j9bcv_recordClassRelationshipSnippet(J9BytecodeVerificationData *verifyData, UDATA childClassNameIndex, UDATA parentClassNameIndex, IDATA *reasonCode)
{
	J9VMThread *vmThread = verifyData->vmStruct;
	J9ClassRelationshipSnippet *snippetEntry = NULL;
	IDATA recordResult = FALSE;
	J9ClassRelationshipSnippet exemplar = {0};
	*reasonCode = BCV_SUCCESS;

	Trc_RTV_recordClassRelationshipSnippet_Entry(vmThread, childClassNameIndex, parentClassNameIndex);

	/* If the snippet hash table has not been allocated yet, create it */
	if (NULL == verifyData->classRelationshipSnippetsHashTable) {
		*reasonCode = allocateClassRelationshipSnippetsHashTable(verifyData);

		if (BCV_SUCCESS != *reasonCode) {
			goto doneRecordSnippet;
		}
	}

	exemplar.childClassNameIndex = childClassNameIndex;
	exemplar.parentClassNameIndex = parentClassNameIndex;
	snippetEntry = hashTableFind(verifyData->classRelationshipSnippetsHashTable, &exemplar);

	if (NULL == snippetEntry) {
		snippetEntry = hashTableAdd(verifyData->classRelationshipSnippetsHashTable, &exemplar);

		if (NULL == snippetEntry) {
			Trc_RTV_recordClassRelationshipSnippet_EntryAllocationFailed(vmThread);
			*reasonCode = BCV_ERR_INSUFFICIENT_MEMORY;
		} else {
			recordResult = TRUE;
		}
	}

doneRecordSnippet:
	Trc_RTV_recordClassRelationshipSnippet_Exit(vmThread, snippetEntry, *reasonCode);
	return recordResult;
}

/**
 * Process class relationship snippets for a ROM class.
 *
 * Validate a relationship if both the child class and the parent class are already loaded.
 * If a parent class is loaded and is an interface, be permissive.
 * Otherwise, record the relationship in the class relationships table for deferred validation.
 *
 * Returns BCV_SUCCESS on success
 * Returns BCV_ERR_INTERNAL_ERROR on error
 * Returns BCV_ERR_INSUFFICIENT_MEMORY on OOM
 */
IDATA
j9bcv_processClassRelationshipSnippets(J9BytecodeVerificationData *verifyData, J9SharedDataDescriptor *snippetsDataDescriptor)
{
	J9VMThread *vmThread = verifyData->vmStruct;
	IDATA processResult = BCV_SUCCESS;
	uint8_t *snippetsDataDescriptorAddress = (uint8_t *) snippetsDataDescriptor->address;

	Trc_RTV_processClassRelationshipSnippets_Entry(vmThread, (UDATA) J9UTF8_LENGTH((J9ROMCLASS_CLASSNAME(verifyData->romClass))), J9UTF8_DATA(J9ROMCLASS_CLASSNAME(verifyData->romClass)));

	if (NULL != snippetsDataDescriptorAddress) {
		/* Subsequent run/Use cached data: Process snippets from cached data descriptor */
		Trc_RTV_processClassRelationshipSnippets_UsingCachedData(vmThread);
		processClassRelationshipSnippetsUsingCachedData(verifyData, snippetsDataDescriptorAddress, &processResult);
	} else {
		if (NULL == verifyData->classRelationshipSnippetsHashTable) {
			/* No snippets were stored for this romClass */
		} else {
			/* Initial run/No cached snippets: Process snippets using local hash table */
			Trc_RTV_processClassRelationshipSnippets_NoCachedData(vmThread);
			processClassRelationshipSnippetsNoCachedData(verifyData, &processResult);
			freeClassRelationshipSnippetsHashTable(verifyData);
		}
	}

	Trc_RTV_processClassRelationshipSnippets_Exit(vmThread, processResult);
	return processResult;
}

/**
 * Validate class relationships for snippets in the verification data snippet hash table.
 */
static void
processClassRelationshipSnippetsNoCachedData(J9BytecodeVerificationData *verifyData, IDATA *reasonCode)
{
	J9VMThread *vmThread = verifyData->vmStruct;
	J9HashTableState hashTableState = {0};
	J9ClassRelationshipSnippet *snippetEntry = (J9ClassRelationshipSnippet *) hashTableStartDo(verifyData->classRelationshipSnippetsHashTable, &hashTableState);

	while (NULL != snippetEntry) {
		U_8 *childClassName = NULL;
		U_8 *parentClassName = NULL;
		UDATA childClassNameLength = 0;
		UDATA parentClassNameLength = 0;
		
		getNameAndLengthFromClassNameList(verifyData, snippetEntry->childClassNameIndex, &childClassName, &childClassNameLength);
		getNameAndLengthFromClassNameList(verifyData, snippetEntry->parentClassNameIndex, &parentClassName, &parentClassNameLength);

		checkSnippetRelationship(verifyData, childClassName, childClassNameLength, parentClassName, parentClassNameLength, reasonCode);

		if (BCV_SUCCESS != *reasonCode) {
			/* Either an OOM or verification error occurred while processing snippets */
			Trc_RTV_processClassRelationshipSnippets_ErrorWhileProcessing(vmThread, childClassNameLength, childClassName, parentClassNameLength, parentClassName);
			break;
		}

		snippetEntry = (J9ClassRelationshipSnippet *) hashTableNextDo(&hashTableState);
	}

	return;
}

/**
 * Validate class relationships for snippets retrieved from the SCC.
 */
static void
processClassRelationshipSnippetsUsingCachedData(J9BytecodeVerificationData *verifyData, uint8_t *snippetsDataDescriptorAddress, IDATA *reasonCode)
{
	J9VMThread *vmThread = verifyData->vmStruct;
	UDATA headerSize = sizeof(J9SharedClassRelationshipHeader);
	UDATA *cacheDataHeaderStart = (UDATA *) snippetsDataDescriptorAddress;
	UDATA snippetCount = *cacheDataHeaderStart;
	J9SharedClassRelationshipSnippet *cacheDataSnippets = (J9SharedClassRelationshipSnippet *) (snippetsDataDescriptorAddress + headerSize);
	UDATA i = 0;

	for (; i < snippetCount; i++) {
		J9UTF8 *childClassUTF8 = NULL;
		J9UTF8 *parentClassUTF8 = NULL;
		U_8 *childClassName = NULL;
		U_8 *parentClassName = NULL;
		UDATA childClassNameLength = 0;
		UDATA parentClassNameLength = 0;

		childClassUTF8 = SRP_GET(cacheDataSnippets[i].childClassName, J9UTF8 *);
		Assert_RTV_true(NULL != childClassUTF8);
		parentClassUTF8 = SRP_GET(cacheDataSnippets[i].parentClassName, J9UTF8 *);
		Assert_RTV_true(NULL != parentClassUTF8);

		childClassName = J9UTF8_DATA(childClassUTF8);
		parentClassName = J9UTF8_DATA(parentClassUTF8);
		childClassNameLength = J9UTF8_LENGTH(childClassUTF8);
		parentClassNameLength = J9UTF8_LENGTH(parentClassUTF8);

		checkSnippetRelationship(verifyData, childClassName, childClassNameLength, parentClassName, parentClassNameLength, reasonCode);
	
		if (BCV_SUCCESS != *reasonCode) {
			/* Either an OOM or verification error occurred while processing snippets */
			Trc_RTV_processClassRelationshipSnippets_ErrorWhileProcessing(vmThread, childClassNameLength, childClassName, parentClassNameLength, parentClassName);
			break;
		}
	}
	
	return;
}

/**
 * Validate a relationship between two loaded classes in a snippet.
 * 
 * If either of the classes are not loaded, record the relationship in the
 * class loader's hash table.
 */
static void
checkSnippetRelationship(J9BytecodeVerificationData *verifyData, U_8 *childClassName, UDATA childClassNameLength, U_8 *parentClassName, UDATA parentClassNameLength, IDATA *reasonCode)
{
	J9VMThread *vmThread = verifyData->vmStruct;
	J9JavaVM *vm = vmThread->javaVM;
	J9ClassLoader *classLoader = verifyData->classLoader;
	J9Class *childClass = NULL;
	J9Class *parentClass = NULL;
	*reasonCode = BCV_SUCCESS;

	/* Check if the parentClass is already loaded */
	parentClass = vm->internalVMFunctions->hashClassTableAt(classLoader, parentClassName, parentClassNameLength);

	/* If parentClass is not already loaded, record the relationship */
	if (NULL == parentClass) {
		j9bcv_recordClassRelationship(vmThread, classLoader, childClassName, childClassNameLength, parentClassName, parentClassNameLength, reasonCode);
		goto donecheckSnippetRelationship;
	} else if (J9ROMCLASS_IS_INTERFACE(parentClass->romClass)) {
		/* Relationship verification passes; don't save relationship */
		Trc_RTV_validateClassRelationships_ParentClassIsInterface(vmThread, parentClassNameLength, parentClassName, NULL);
		goto donecheckSnippetRelationship;
	}

	/* Check if the childClass is already loaded */
	childClass = vm->internalVMFunctions->hashClassTableAt(classLoader, childClassName, childClassNameLength);

	/* If childClass is not already loaded, record the relationship */
	if (NULL == childClass) {
		j9bcv_recordClassRelationship(vmThread, classLoader, childClassName, childClassNameLength, parentClassName, parentClassNameLength, reasonCode);
		goto donecheckSnippetRelationship;
	}
	
	/* Both the child class and the parent class are already loaded; verify their relationship */
	if (isSameOrSuperClassOf(parentClass, childClass)) {
		Trc_RTV_validateClassRelationships_ParentClassIsSuperClass(vmThread, parentClassNameLength, parentClassName, NULL);
	} else {
		/* The child and parent have an invalid relationship - they are not compatible classes */
		Trc_RTV_validateClassRelationships_InvalidRelationship(vmThread, parentClassNameLength, parentClassName);
		*reasonCode = BCV_ERR_INTERNAL_ERROR;
	}

donecheckSnippetRelationship:
	return;
}

/**
 * Generates a key for a set of class relationship snippets corresponding to a particular class.
 *
 * Returns a string that identifies the class, or NULL if an error occurs.
 */
static char *
generateClassRelationshipSnippetsKey(J9JavaVM *vm, J9VMThread *vmThread, U_8 *className, UDATA classNameLength)
{
	PORT_ACCESS_FROM_JAVAVM(vm);
	UDATA keyLength = classNameLength + 1;
	char *key = (char *) j9mem_allocate_memory(keyLength, J9MEM_CATEGORY_CLASSES_CRV_SNIPPETS);

	if (NULL != key) {
		memcpy(key, className, classNameLength);
		key[classNameLength] = '\0';
	} else {
		Trc_RTV_generateClassRelationshipSnippetsKey_GenerateKeyFailed(vmThread, classNameLength, className);
	}

	return key;
}

/**
 * Store class relationship snippets for a ROM class to the Shared Classes Cache.
 *
 * Data Descriptor Format
 *   ------------  <- dataBufferHeaderStart
 *  |   HEADER   |    J9SharedClassRelationshipHeader - UDATA snippetCount
 *  | ---------- | <- dataBufferSnippetStart
 *  |  SNIPPETS  |    J9SharedClassRelationshipSnippet - {J9SRP, J9SRP}
 *  | ---------- | <- dataBufferUTF8Start
 *  | CLASSNAMES |    J9UTF8 - "ClassName"
 *   ------------
 *
 * Returns BCV_SUCCESS on success
 * Returns BCV_ERR_INSUFFICIENT_MEMORY on OOM
 * Returns BCV_ERR_INTERNAL_ERROR on error
 */
IDATA
j9bcv_storeClassRelationshipSnippetsToSharedCache(J9BytecodeVerificationData *verifyData)
{
	J9JavaVM *vm = verifyData->javaVM;
	J9VMThread *vmThread = verifyData->vmStruct;
	J9ROMClass *romClass = verifyData->romClass;
	PORT_ACCESS_FROM_JAVAVM(vm);
	UDATA snippetCount = 0;
	char *key = NULL;
	IDATA storeResult = BCV_SUCCESS;
	U_8 *className = J9UTF8_DATA(J9ROMCLASS_CLASSNAME(romClass));
	UDATA classNameLength = (UDATA) J9UTF8_LENGTH((J9ROMCLASS_CLASSNAME(romClass)));

	Trc_RTV_storeClassRelationshipSnippetsToSharedCache_Entry(vmThread, classNameLength, className);

	if (NULL != verifyData->classRelationshipSnippetsHashTable) {
		snippetCount = hashTableGetCount(verifyData->classRelationshipSnippetsHashTable);
	}

	if (0 == snippetCount) {
		Trc_RTV_storeClassRelationshipSnippetsToSharedCache_NoSnippets(vmThread);
		goto doneStoreSnippets;
	}

	key = generateClassRelationshipSnippetsKey(vm, vmThread, className, classNameLength);

	if (NULL != key) {
		/* Pointers to data sections */
		UDATA *dataBufferHeaderStart = NULL;
		J9SharedClassRelationshipSnippet *dataBufferSnippetStart = NULL;
		J9UTF8 *dataBufferUTF8Start = NULL;

		/* Data size calculations */
		UDATA headerSize = sizeof(J9SharedClassRelationshipHeader);
		UDATA snippetSize = sizeof(J9SharedClassRelationshipSnippet);
		UDATA snippetsSizeTotal = snippetCount * snippetSize;
		UDATA utf8SizeTotal = getTotalUTF8Size(verifyData);
		UDATA dataBufferSize = headerSize + snippetsSizeTotal + utf8SizeTotal;

		uint8_t *dataBuffer = (uint8_t *) j9mem_allocate_memory(dataBufferSize, J9MEM_CATEGORY_CLASSES_CRV_SNIPPETS);

		Trc_RTV_storeClassRelationshipSnippetsToSharedCache_Allocation(vmThread, dataBufferSize, snippetCount);

		if (NULL == dataBuffer) {
			Trc_RTV_storeClassRelationshipSnippetsToSharedCache_AllocationFailed(vmThread);
			storeResult = BCV_ERR_INSUFFICIENT_MEMORY;

			goto doneStoreSnippets;
		}

		memset(dataBuffer, 0, dataBufferSize);
		dataBufferHeaderStart = (UDATA *) dataBuffer;
		dataBufferSnippetStart = (J9SharedClassRelationshipSnippet *) (dataBuffer + headerSize);
		dataBufferUTF8Start = (J9UTF8 *) (dataBuffer + headerSize + snippetsSizeTotal);

		/* Store snippet count in header section of dataBuffer */
		*dataBufferHeaderStart = snippetCount;

		/* Store J9SharedClassRelationshipSnippets and J9UTF8s in dataBuffer */
		storeResult = storeToDataBuffer(verifyData, dataBuffer, dataBufferSnippetStart, dataBufferUTF8Start, snippetCount);

		if (BCV_SUCCESS == storeResult) {
			J9SharedClassConfig *sharedClassConfig = vm->sharedClassConfig;
			J9SharedDataDescriptor dataDescriptor = {0};
			const U_8 *sccLocation = NULL;

			/* Set data descriptor fields and store data to SCC */
			dataDescriptor.address = (U_8 *) dataBuffer;
			dataDescriptor.length = dataBufferSize;
			dataDescriptor.flags = J9SHRDATA_SINGLE_STORE_FOR_KEY_TYPE;
			dataDescriptor.type = J9SHR_DATA_TYPE_CRVSNIPPET;

			sccLocation = sharedClassConfig->storeSharedData(vmThread, key, strlen(key), &dataDescriptor);

			if (NULL == sccLocation) {
				Trc_RTV_storeClassRelationshipSnippetsToSharedCache_StoreFailed(vmThread);
				storeResult = BCV_ERR_INTERNAL_ERROR;
			} else {
				Trc_RTV_storeClassRelationshipSnippetsToSharedCache_StoreSuccessful(vmThread, sccLocation);
			}
		} else {
			Trc_RTV_storeClassRelationshipSnippetsToSharedCache_StoreFailed(vmThread);
		}

		j9mem_free_memory(dataBuffer);
	} else {
		Trc_RTV_storeClassRelationshipSnippetsToSharedCache_StoreFailed(vmThread);
		storeResult = BCV_ERR_INTERNAL_ERROR;
	}

doneStoreSnippets:
	j9mem_free_memory(key);
	Trc_RTV_storeClassRelationshipSnippetsToSharedCache_Exit(vmThread, storeResult);

	return storeResult;
}

/**
 * Fetch class relationship snippets for a ROM class from the Shared Classes Cache.
 *
 * Returns TRUE if snippets for the ROM class are found in the cache, otherwise returns FALSE
 */
BOOLEAN
j9bcv_fetchClassRelationshipSnippetsFromSharedCache(J9BytecodeVerificationData *verifyData, J9SharedDataDescriptor *snippetsDataDescriptor, IDATA *fetchResult)
{
	J9JavaVM *vm = verifyData->javaVM;
	J9VMThread *vmThread = verifyData->vmStruct;
	J9ROMClass *romClass = verifyData->romClass;
	PORT_ACCESS_FROM_JAVAVM(vm);
	J9SharedClassConfig *sharedClassConfig = vm->sharedClassConfig;
	IDATA findSharedDataResult = -1;
	BOOLEAN foundSnippets = FALSE;
	U_8 *className = J9UTF8_DATA(J9ROMCLASS_CLASSNAME(romClass));
	UDATA classNameLength = (UDATA) J9UTF8_LENGTH((J9ROMCLASS_CLASSNAME(romClass)));
	char *key = generateClassRelationshipSnippetsKey(vm, vmThread, className, classNameLength);
	*fetchResult = BCV_SUCCESS;

	Trc_RTV_fetchClassRelationshipSnippetsFromSharedCache_Entry(vmThread, classNameLength, className);

	if (NULL != key) {
		UDATA keyLength = strlen(key);
		/* findSharedData() returns the number of data elements found or -1 in the case of error */
		findSharedDataResult = sharedClassConfig->findSharedData(vmThread, key, keyLength, J9SHR_DATA_TYPE_CRVSNIPPET, 0, snippetsDataDescriptor, NULL);

		if (findSharedDataResult > 0) {
			/* Subsequent run: snippets already exist in shared cache */
			Trc_RTV_fetchClassRelationshipSnippetsFromSharedCache_FoundSnippets(vmThread);
			foundSnippets = TRUE;
		} else {
			/* Initial run: use hashtable in verifyData to hold snippets to be stored to shared cache */
			snippetsDataDescriptor = NULL;

			if (-1 == findSharedDataResult) {
				Trc_RTV_fetchClassRelationshipSnippetsFromSharedCache_Error(vmThread);
				*fetchResult = BCV_ERR_INTERNAL_ERROR;
			}
		}
	} else {
		Trc_RTV_fetchClassRelationshipSnippetsFromSharedCache_Error(vmThread);
		*fetchResult = BCV_ERR_INTERNAL_ERROR;
	}

	j9mem_free_memory(key);
	Trc_RTV_fetchClassRelationshipSnippetsFromSharedCache_Exit(vmThread, findSharedDataResult, *fetchResult);

	return foundSnippets;
}

/**
 * Allocates new hash table to store class relationship snippet entries.
 *
 * Returns BCV_SUCCESS if successful and BCV_ERR_INSUFFICIENT_MEMORY on OOM.
 */
static IDATA
allocateClassRelationshipSnippetsHashTable(J9BytecodeVerificationData *verifyData)
{
	J9JavaVM *vm = verifyData->javaVM;
	IDATA reasonCode = BCV_SUCCESS;

	verifyData->classRelationshipSnippetsHashTable = hashTableNew(OMRPORT_FROM_J9PORT(vm->portLibrary), J9_GET_CALLSITE(), 100, sizeof(J9ClassRelationshipSnippet), 0, 0, J9MEM_CATEGORY_CLASSES_CRV_SNIPPETS, relationshipSnippetHashFn, relationshipSnippetHashEqualFn, NULL, vm);

	if (NULL == verifyData->classRelationshipSnippetsHashTable) {
		reasonCode = BCV_ERR_INSUFFICIENT_MEMORY;
	}

	return reasonCode;
}

/**
 * Frees memory for each J9ClassRelationshipSnippet table entry
 * and the classRelationshipSnippets hash table itself.
 */
static void
freeClassRelationshipSnippetsHashTable(J9BytecodeVerificationData *verifyData)
{
	if (NULL != verifyData->classRelationshipSnippetsHashTable) {
		J9HashTableState hashTableState = {0};
		J9ClassRelationshipSnippet *snippetEntryStart = (J9ClassRelationshipSnippet *) hashTableStartDo(verifyData->classRelationshipSnippetsHashTable, &hashTableState);
		J9ClassRelationshipSnippet *snippetEntry = snippetEntryStart;

		while (NULL != snippetEntry) {
			UDATA result = hashTableDoRemove(&hashTableState);
			Assert_RTV_true(0 == result);
			snippetEntry = (J9ClassRelationshipSnippet *) hashTableNextDo(&hashTableState);
		}

		hashTableFree(verifyData->classRelationshipSnippetsHashTable);
		verifyData->classRelationshipSnippetsHashTable = NULL;
	}

	return;
}

/**
 * Hash function for J9ClassRelationshipSnippet entries used to keep
 * track of class relationship snippets before storing them to the SCC.
 */
static UDATA
relationshipSnippetHashFn(void *key, void *userData)
{
	J9ClassRelationshipSnippet *relationshipSnippetKey = key;
	J9JavaVM *vm = userData;

	UDATA utf8HashChildSnippet = (UDATA) convertValueToHash(vm, relationshipSnippetKey->childClassNameIndex);
	UDATA utf8HashChildXORParentSnippet = utf8HashChildSnippet ^ relationshipSnippetKey->parentClassNameIndex;
	UDATA utf8HashSnippet = (UDATA) convertValueToHash(vm, utf8HashChildXORParentSnippet);

	return utf8HashSnippet;
}

/**
 * Hash equal function for J9ClassRelationshipSnippet entries used to keep
 * track of class relationship snippets before storing them to the SCC.
 */
static UDATA
relationshipSnippetHashEqualFn(void *leftKey, void *rightKey, void *userData)
{
	J9ClassRelationshipSnippet *left_relationshipSnippetKey = leftKey;
	J9ClassRelationshipSnippet *right_relationshipSnippetKey = rightKey;

	UDATA snippetchildClassNameIndexEqual = left_relationshipSnippetKey->childClassNameIndex == right_relationshipSnippetKey->childClassNameIndex;
	UDATA snippetparentClassNameIndexEqual = left_relationshipSnippetKey->parentClassNameIndex == right_relationshipSnippetKey->parentClassNameIndex;
	UDATA snippetNameEqual = snippetchildClassNameIndexEqual && snippetparentClassNameIndexEqual;

	return snippetNameEqual;
}

/**
 * Store class name mappings to local data buffer.
 * 
 * Use hashtable for storage if (snippetCount > J9RELATIONSHIP_SNIPPET_COUNT_THRESHOLD).
 * Use array for storage if (snippetCount <= J9RELATIONSHIP_SNIPPET_COUNT_THRESHOLD).
 */
static IDATA
storeToDataBuffer(J9BytecodeVerificationData *verifyData, uint8_t *dataBuffer, J9SharedClassRelationshipSnippet *dataBufferSnippetStart, J9UTF8 *dataBufferUTF8Start, UDATA snippetCount)
{
	J9JavaVM *vm = verifyData->javaVM;
	J9VMThread *vmThread = verifyData->vmStruct;
	J9SRP *srpAddress = (J9SRP *) dataBufferSnippetStart;
	J9UTF8 *nextUTF8Address = dataBufferUTF8Start;
	UDATA offsetToNextSRP = sizeof(J9SRP);
	J9HashTable *classRelationshipSnippetsHashTable = verifyData->classRelationshipSnippetsHashTable;
	J9HashTableState hashTableState = {0};
	J9ClassRelationshipSnippet *snippetEntry = NULL;
	IDATA storeResult = BCV_SUCCESS;
	UDATA snippetConfig = J9RELATIONSHIP_SNIPPET_SINGLE;
	J9HashTable *classNamesHashTable = NULL;
	UDATA totalNumberOfIndices = snippetCount * 2;
	J9ClassRelationshipClassNameIndex *classNamesArray[J9RELATIONSHIP_SNIPPET_COUNT_THRESHOLD * 2] = {0};

	if (1 != snippetCount) {
		if (snippetCount > J9RELATIONSHIP_SNIPPET_COUNT_THRESHOLD) {
			/* Use hashtable to store class name mappings */
			Trc_RTV_storeToDataBuffer_Hashtable(vmThread);

			snippetConfig = J9RELATIONSHIP_SNIPPET_USE_HASHTABLE;
			classNamesHashTable = hashRelationshipClassNameTableNew(vm);

			if (NULL == classNamesHashTable) {
				Trc_RTV_storeToDataBuffer_HashtableAllocationFailed(vmThread);
				storeResult = BCV_ERR_INSUFFICIENT_MEMORY;
				goto doneStoreToDataBuffer;
			}
		} else {
			/* Use array to store class name mappings */
			Trc_RTV_storeToDataBuffer_Array(vmThread);
			snippetConfig = J9RELATIONSHIP_SNIPPET_USE_ARRAY;
		}
	}

	snippetEntry = (J9ClassRelationshipSnippet *) hashTableStartDo(classRelationshipSnippetsHashTable, &hashTableState);

	/* Store J9UTF8s to the data buffer and set SRPs */
	while (NULL != snippetEntry) {
		J9UTF8 *childClassUTF8Address = NULL;
		J9UTF8 *parentClassUTF8Address = NULL;

		if (J9RELATIONSHIP_SNIPPET_USE_HASHTABLE == snippetConfig) {
			childClassUTF8Address = getUTF8AddressFromHashTable(verifyData, &nextUTF8Address, classNamesHashTable, snippetEntry->childClassNameIndex);
		} else if (J9RELATIONSHIP_SNIPPET_USE_ARRAY == snippetConfig) {
			childClassUTF8Address = getUTF8AddressFromArray(verifyData, &nextUTF8Address, classNamesArray, totalNumberOfIndices, snippetEntry->childClassNameIndex);
		} else {
			childClassUTF8Address = getUTF8Address(verifyData, &nextUTF8Address, snippetEntry->childClassNameIndex);
		}

		if (NULL == childClassUTF8Address) {
			Trc_RTV_storeToDataBuffer_getUTF8AddressFailed(vmThread, snippetEntry->childClassNameIndex);
			storeResult = BCV_ERR_INSUFFICIENT_MEMORY;
			break;
		}

		if (J9RELATIONSHIP_SNIPPET_USE_HASHTABLE == snippetConfig) {
			parentClassUTF8Address = getUTF8AddressFromHashTable(verifyData, &nextUTF8Address, classNamesHashTable, snippetEntry->parentClassNameIndex);
		} else if (J9RELATIONSHIP_SNIPPET_USE_ARRAY == snippetConfig) {
			parentClassUTF8Address = getUTF8AddressFromArray(verifyData, &nextUTF8Address, classNamesArray, totalNumberOfIndices, snippetEntry->parentClassNameIndex);
		} else {
			parentClassUTF8Address = getUTF8Address(verifyData, &nextUTF8Address, snippetEntry->parentClassNameIndex);
		}

		if (NULL == parentClassUTF8Address) {
			Trc_RTV_storeToDataBuffer_getUTF8AddressFailed(vmThread, snippetEntry->parentClassNameIndex);
			storeResult = BCV_ERR_INSUFFICIENT_MEMORY;
			break;
		}

		SRP_PTR_SET(srpAddress, childClassUTF8Address);
		srpAddress = (J9SRP *) ((uint8_t *) srpAddress + offsetToNextSRP);

		SRP_PTR_SET(srpAddress, parentClassUTF8Address);
		srpAddress = (J9SRP *) ((uint8_t *) srpAddress + offsetToNextSRP);

		Trc_RTV_storeToDataBuffer_StoredSnippet(vmThread, J9UTF8_LENGTH(childClassUTF8Address), J9UTF8_DATA(childClassUTF8Address), J9UTF8_LENGTH(parentClassUTF8Address), J9UTF8_DATA(parentClassUTF8Address));

		snippetEntry = (J9ClassRelationshipSnippet *) hashTableNextDo(&hashTableState);
	}

doneStoreToDataBuffer:
	/* Free hashtable if used to store class name mappings */
	if (J9RELATIONSHIP_SNIPPET_USE_HASHTABLE == snippetConfig) {
		hashClassRelationshipClassNameTableFree(vmThread, classNamesHashTable);
	}

	return storeResult;
}

/**
 * Writes a class name to the next availble J9UTF8 address in the data buffer.
 * 
 * Returns the size of data stored at the J9UTF8 address.
 */
static UDATA
setCurrentAndNextUTF8s(J9BytecodeVerificationData *verifyData, J9UTF8 **utf8Address, J9UTF8 **nextUTF8Address, UDATA classNameIndex)
{
	J9UTF8 *classNameUTF8 = NULL;
	U_8 *className = NULL;
	UDATA classNameLength = 0;
	UDATA utf8AddressSize = 0;
	PORT_ACCESS_FROM_JAVAVM(verifyData->javaVM);

	getNameAndLengthFromClassNameList(verifyData, classNameIndex, &className, &classNameLength);
	
	*utf8Address = *nextUTF8Address;
	J9UTF8_SET_LENGTH(*utf8Address, (U_16) classNameLength);
	memcpy(J9UTF8_DATA(*utf8Address), className, classNameLength);
	J9UTF8_DATA(*utf8Address)[classNameLength] = '\0';

	utf8AddressSize = classNameLength + 1 + sizeof((U_16) classNameLength);

	return utf8AddressSize;
}

/**
 * Allocates a J9UTF8 address in the data buffer for a class name.
 *
 * Returns the J9UTF8 address where the class name is stored in the data buffer.
 */
static J9UTF8 *
getUTF8Address(J9BytecodeVerificationData *verifyData, J9UTF8 **nextUTF8Address, UDATA classNameIndex)
{
	J9UTF8 *utf8Address = NULL;

	UDATA utf8AddressSize = setCurrentAndNextUTF8s(verifyData, &utf8Address, nextUTF8Address, classNameIndex);

	/* Update pointer to the next address where a class name can be stored */
	*nextUTF8Address = (J9UTF8 *) ((uint8_t *) utf8Address + utf8AddressSize);

	return utf8Address;
}

/**
 * Retrieves or allocates the J9UTF8 address in the data buffer for a class name
 * using the classNamesArray.
 *
 * If the array does not contain an entry for the class name, store the new class
 * name in the buffer and add the entry to the array with the J9UTF8 address.
 *
 * Returns the J9UTF8 address where the class name is stored in the data buffer.
 */
static J9UTF8 *
getUTF8AddressFromArray(J9BytecodeVerificationData *verifyData, J9UTF8 **nextUTF8Address, J9ClassRelationshipClassNameIndex **classNamesArray, UDATA totalNumberOfIndices, UDATA classNameIndex)
{
	J9UTF8 *utf8Address = NULL;
	BOOLEAN foundExisting = FALSE;
	UDATA i = 0;

	for (; i < totalNumberOfIndices; i++) {
		if (NULL == classNamesArray[i]) {
			break;
		}

		if (classNameIndex == classNamesArray[i]->classNameIndex) {
			/* Class name has already been allocated, return existing address */
			foundExisting = TRUE;
			utf8Address = (J9UTF8 *) classNamesArray[i]->address;
			break;
		}
	}

	if (!foundExisting) {
		PORT_ACCESS_FROM_JAVAVM(verifyData->javaVM);
		J9ClassRelationshipClassNameIndex *classNameIndexEntry = (J9ClassRelationshipClassNameIndex *) j9mem_allocate_memory(sizeof(J9ClassRelationshipClassNameIndex), J9MEM_CATEGORY_CLASSES_CRV_SNIPPETS);

		if (NULL != classNameIndexEntry) {
			UDATA utf8AddressSize = setCurrentAndNextUTF8s(verifyData, &utf8Address, nextUTF8Address, classNameIndex);

			if (NULL != utf8Address) {
				/* Add the class name to address mapping to the array */
				classNameIndexEntry->classNameIndex = classNameIndex;
				classNameIndexEntry->address = (U_32 *) utf8Address;
				classNamesArray[i] = classNameIndexEntry;

				/* Update pointer to the next address where a class name can be stored */
				*nextUTF8Address = (J9UTF8 *) ((uint8_t *) utf8Address + utf8AddressSize);
			}
		}
	}

	return utf8Address;
}

/**
 * Retrieves or allocates the J9UTF8 address in the data buffer for a class name
 * using the classNamesHashTable.
 *
 * If the hash table does not contain an entry for the class name, store the new
 * class name in the buffer and create a new table entry with the J9UTF8 address.
 *
 * Returns the J9UTF8 address where the class name is stored in the data buffer.
 */
static J9UTF8 *
getUTF8AddressFromHashTable(J9BytecodeVerificationData *verifyData, J9UTF8 **nextUTF8Address, J9HashTable *classNamesHashTable, UDATA classNameIndex)
{
	J9UTF8 *utf8Address = NULL;
	UDATA utf8AddressSize = setCurrentAndNextUTF8s(verifyData, &utf8Address, nextUTF8Address, classNameIndex);
	J9ClassRelationshipClassName classNameExemplar = {0};

	classNameExemplar.utf8 = utf8Address;

	if (NULL != classNameExemplar.utf8) {
		/* Check if there is an existing entry with the same class name */
		J9ClassRelationshipClassName *classNameEntry = (J9ClassRelationshipClassName *) hashTableFind(classNamesHashTable, &classNameExemplar);

		if (NULL == classNameEntry) {
			/* There is no existing entry; add the J9UTF8 address to the hash table for future lookup */
			classNameExemplar.address = (U_32 *) utf8Address;
			classNameEntry = hashTableAdd(classNamesHashTable, &classNameExemplar);
			Assert_RTV_true(NULL != classNameEntry);

			/* Update pointer to the next address where a class name can be stored */
			*nextUTF8Address = (J9UTF8 *) ((uint8_t *) utf8Address + utf8AddressSize);
		} else {
			PORT_ACCESS_FROM_JAVAVM(verifyData->javaVM);
			/* Use the J9UTF8 address that was previously stored */
			memset(utf8Address, 0, utf8AddressSize);
			utf8Address = (J9UTF8 *) classNameEntry->address;
		}
	}

	return utf8Address;
}

/**
 * Calculates the total size needed to store each unique class name.
 * 
 * Returns the amount of memory needed for class name UTF8s.
 */
static UDATA
getTotalUTF8Size(J9BytecodeVerificationData *verifyData) {
	UDATA size = 0;
	J9UTF8 **classNameList = verifyData->classNameList;
	J9HashTable *classRelationshipSnippetsHashTable = verifyData->classRelationshipSnippetsHashTable;
	J9HashTableState hashTableState = {0};
	J9ClassRelationshipSnippet *snippetEntry = (J9ClassRelationshipSnippet *) hashTableStartDo(classRelationshipSnippetsHashTable, &hashTableState);
	U_16 childClassNameLength = 0;
	U_16 parentClassNameLength = 0;

	while (NULL != snippetEntry) {
		childClassNameLength = J9UTF8_LENGTH(classNameList[snippetEntry->childClassNameIndex] + 1) + 1;
		parentClassNameLength = J9UTF8_LENGTH(classNameList[snippetEntry->parentClassNameIndex] + 1) + 1;
		size += childClassNameLength + sizeof(childClassNameLength) + parentClassNameLength + sizeof(parentClassNameLength);

		snippetEntry = (J9ClassRelationshipSnippet *) hashTableNextDo(&hashTableState);
	}

	return size;
}

/**
 * Allocates new hash table to store class relationship snippet class names.
 *
 * Returns the new hash table, or NULL if the creation fails.
 */
static J9HashTable *
hashRelationshipClassNameTableNew(J9JavaVM *vm)
{
	J9HashTable *relationshipClassNameHashTable = hashTableNew(OMRPORT_FROM_J9PORT(vm->portLibrary), J9_GET_CALLSITE(), 100, sizeof(J9ClassRelationshipClassName), 0, 0, J9MEM_CATEGORY_CLASSES_CRV_SNIPPETS, relationshipClassNameHashFn, relationshipClassNameHashEqualFn, NULL, vm);

	return relationshipClassNameHashTable;
}

/**
 * Frees memory for each J9ClassRelationshipClassName table entry, each entry's utf8
 * and the relationshipClassName hash table itself.
 */
static void
hashClassRelationshipClassNameTableFree(J9VMThread *vmThread, J9HashTable *relationshipClassNameHashTable)
{
	UDATA result = 0;
	J9HashTableState hashTableState = {0};
	J9ClassRelationshipClassName *classNameEntry = (J9ClassRelationshipClassName *) hashTableStartDo(relationshipClassNameHashTable, &hashTableState);

	while (NULL != classNameEntry) {
		result = hashTableDoRemove(&hashTableState);
		Assert_RTV_true(0 == result);
		classNameEntry = (J9ClassRelationshipClassName *) hashTableNextDo(&hashTableState);
	}

	hashTableFree(relationshipClassNameHashTable);
	relationshipClassNameHashTable = NULL;

	return;
}

/**
 * Hash function for J9ClassRelationshipClassName entries used to map
 * class names to local data buffer.
 */
static UDATA
relationshipClassNameHashFn(void *key, void *userData)
{
	J9ClassRelationshipClassName *classNameKey = key;
	J9UTF8 *classNameUTF8 = classNameKey->utf8;
	J9JavaVM *vm = userData;

	UDATA utf8HashClassName = J9_VM_FUNCTION_VIA_JAVAVM(vm, computeHashForUTF8)(J9UTF8_DATA(classNameUTF8), J9UTF8_LENGTH(classNameUTF8));

	return utf8HashClassName;
}

/**
 * Hash equal function for J9ClassRelationshipClassName entries used to map
 * class names to local data buffer.
 */
static UDATA
relationshipClassNameHashEqualFn(void *leftKey, void *rightKey, void *userData)
{
	J9ClassRelationshipClassName *left_classNameKey = leftKey;
	J9ClassRelationshipClassName *right_classNameKey = rightKey;
	J9UTF8 *left_classNameUTF8 = left_classNameKey->utf8;
	J9UTF8 *right_classNameUTF8 = right_classNameKey->utf8;

	UDATA classNameEqual = J9UTF8_DATA_EQUALS(J9UTF8_DATA(left_classNameUTF8), J9UTF8_LENGTH(left_classNameUTF8), J9UTF8_DATA(right_classNameUTF8), J9UTF8_LENGTH(right_classNameUTF8));

	return classNameEqual;
}

/**
 * Class Relationship APIs (J9ClassRelationship)
 */

/**
 * Record a class relationship in the class relationships table.
 *
 * Returns TRUE if successful and FALSE if an out of memory error occurs.
 */
IDATA
j9bcv_recordClassRelationship(J9VMThread *vmThread, J9ClassLoader *classLoader, U_8 *childClassName, UDATA childClassNameLength, U_8 *parentClassName, UDATA parentClassNameLength, IDATA *reasonCode)
{
	J9ClassRelationship *classRelationshipEntry = NULL;
	J9ClassRelationshipNode *parentClassNode = NULL;
	J9ClassRelationship classRelationship = {0};
	IDATA recordResult = FALSE;
	*reasonCode = BCV_ERR_INSUFFICIENT_MEMORY;

	Trc_RTV_recordClassRelationship_Entry(vmThread, childClassNameLength, childClassName, parentClassNameLength, parentClassName);

	Assert_RTV_true((NULL != childClassName) && (NULL != parentClassName));

	/* If the hash table has not been allocated yet, create new hash table and pool */
	if (NULL == classLoader->classRelationshipsHashTable) {
		UDATA allocateResult = allocateClassRelationshipTableAndPool(classLoader, vmThread->javaVM);

		if (0 != allocateResult) {
			goto recordDone;
		}
	}

	/* Locate existing classRelationshipEntry or add new entry to the hashtable */
	classRelationshipEntry = findClassRelationship(vmThread, classLoader, childClassName, childClassNameLength);

	if (NULL == classRelationshipEntry) {
		PORT_ACCESS_FROM_VMC(vmThread);
		classRelationship.className = (U_8 *) j9mem_allocate_memory(childClassNameLength + 1, J9MEM_CATEGORY_CLASSES_CRV_RELATIONSHIPS);

		/* className for classRelationship successfully allocated, continue initialization of classRelationship entry */
		if (NULL != classRelationship.className) {
			memcpy(classRelationship.className, childClassName, childClassNameLength);
			classRelationship.className[childClassNameLength] = '\0';
			classRelationship.classNameLength = childClassNameLength;
			classRelationship.flags = 0;

			classRelationshipEntry = hashTableAdd(classLoader->classRelationshipsHashTable, &classRelationship);

			if (NULL == classRelationshipEntry) {
				Trc_RTV_recordClassRelationship_EntryAllocationFailed(vmThread);
				j9mem_free_memory(classRelationship.className);
				goto recordDone;
			}
		} else {
			Trc_RTV_recordClassRelationship_EntryAllocationFailed(vmThread);
			goto recordDone;
		}
	}

	/* If the parentClass is java/lang/Throwable, set a flag instead of allocating a node */
	if (J9UTF8_DATA_EQUALS(J9RELATIONSHIP_JAVA_LANG_THROWABLE_STRING, J9RELATIONSHIP_JAVA_LANG_THROWABLE_STRING_LENGTH, parentClassName, parentClassNameLength)) {
		if (!J9_ARE_ANY_BITS_SET(classRelationshipEntry->flags, J9RELATIONSHIP_PARENT_CLASS_IS_THROWABLE)) {
			classRelationshipEntry->flags |= J9RELATIONSHIP_PARENT_CLASS_IS_THROWABLE;
		}
	} else {
		/* Add a parentClassNode to the classRelationship's linked list of parent classes */
		if (J9_LINKED_LIST_IS_EMPTY(classRelationshipEntry->root)) {
			parentClassNode = allocateClassRelationshipNode(vmThread, classLoader, parentClassName, parentClassNameLength);
			if (parentClassNode == NULL) {
				/* Allocation failure */
				Trc_RTV_classRelationships_ParentAllocationFailed(vmThread);
				goto recordDone;
			}
			Trc_RTV_recordClassRelationship_AllocatedEntry(vmThread, classRelationshipEntry->classNameLength, classRelationshipEntry->className, classRelationshipEntry, parentClassNode->classNameLength, parentClassNode->className, parentClassNode); 
			J9_LINKED_LIST_ADD_LAST(classRelationshipEntry->root, parentClassNode);
		} else {
			BOOLEAN alreadyPresent = FALSE;
			BOOLEAN addBefore = FALSE;
			J9ClassRelationshipNode *walk = J9_LINKED_LIST_START_DO(classRelationshipEntry->root);
			/**
			 * Keep the list of parentClass nodes ordered by class name length so it's a faster traversal
			 * and duplicates can be avoided
			 */
			while (NULL != walk) {
				if (walk->classNameLength > parentClassNameLength) {
					addBefore = TRUE;
					break;
				} else if (J9UTF8_DATA_EQUALS(walk->className, walk->classNameLength, parentClassName, parentClassNameLength)) {
					/* Already present, skip */
					alreadyPresent = TRUE;
					break;
				} else {
					/* walk->className is shorter or equal length but different data; keep looking */
				}
				walk = J9_LINKED_LIST_NEXT_DO(classRelationshipEntry->root, walk);
			}
			if (!alreadyPresent) {
				parentClassNode = allocateClassRelationshipNode(vmThread, classLoader, parentClassName, parentClassNameLength);
				if (parentClassNode == NULL) {
					/* Allocation failure */
					Trc_RTV_classRelationships_ParentAllocationFailed(vmThread);
					goto recordDone;
				}
				Trc_RTV_recordClassRelationship_AllocatedEntry(vmThread, classRelationshipEntry->classNameLength, classRelationshipEntry->className, classRelationshipEntry, parentClassNode->classNameLength, parentClassNode->className, parentClassNode); 
				if (addBefore) {
					J9_LINKED_LIST_ADD_BEFORE(classRelationshipEntry->root, walk, parentClassNode);
				} else {
					/* If got through the whole list of shorter or equal length names, add it here */
					J9_LINKED_LIST_ADD_LAST(classRelationshipEntry->root, parentClassNode);
				}
			}
		}
	}

	recordResult = TRUE;
	*reasonCode = BCV_SUCCESS;

recordDone:
	Trc_RTV_recordClassRelationship_Exit(vmThread, recordResult);
	return recordResult;
}

/**
 * Validate each recorded relationship for a class (child).
 *
 * Returns failedClass, which is NULL if successful, or the class that fails validation if unsuccessful.
 */
J9Class *
j9bcv_validateClassRelationships(J9VMThread *vmThread, J9ClassLoader *classLoader, U_8 *childClassName, UDATA childClassNameLength, J9Class *childClass)
{
	PORT_ACCESS_FROM_VMC(vmThread);
	J9Class *parentClass = NULL;
	J9Class *failedClass = NULL;
	J9ClassRelationship *classRelationshipEntry = NULL;
	J9ClassRelationshipNode *parentClassNode = NULL;

	Trc_RTV_validateClassRelationships_Entry(vmThread, childClassNameLength, childClassName);
	Assert_RTV_true(NULL != childClassName);
	classRelationshipEntry = findClassRelationship(vmThread, classLoader, childClassName, childClassNameLength);

	/* No relationships were recorded for the class (in this class loader), or its relationships have already been verified */
	if (NULL == classRelationshipEntry) {
		goto validateDone;
	}

	/* The class is invalid if it has been marked as an interface, but it actually isn't */
	if (J9_ARE_ANY_BITS_SET(classRelationshipEntry->flags, J9RELATIONSHIP_MUST_BE_INTERFACE)) {
		Trc_RTV_validateClassRelationships_FlaggedAsInterface(vmThread, childClassNameLength, childClassName);
		if (!J9ROMCLASS_IS_INTERFACE(childClass->romClass)) {
			Trc_RTV_validateClassRelationships_ShouldBeInterface(vmThread, childClassNameLength, childClassName);
			failedClass = childClass;
			goto validateDone;
		}
	}

	/* If J9RELATIONSHIP_PARENT_CLASS_IS_THROWABLE is set, check that the relationship holds */
	if (J9_ARE_ANY_BITS_SET(classRelationshipEntry->flags, J9RELATIONSHIP_PARENT_CLASS_IS_THROWABLE)) {
		/* Throwable will already be loaded since it is a required class J9VMCONSTANTPOOL_JAVALANGTHROWABLE */
		parentClass = J9VMJAVALANGTHROWABLE_OR_NULL(vmThread->javaVM);
		Assert_RTV_true(NULL != parentClass);
		if (isSameOrSuperClassOf(parentClass, childClass)) {
			Trc_RTV_validateClassRelationships_ParentClassIsSuperClass(vmThread, J9RELATIONSHIP_JAVA_LANG_THROWABLE_STRING_LENGTH, J9RELATIONSHIP_JAVA_LANG_THROWABLE_STRING, NULL);
		} else {
			/* The class is invalid since it doesn't hold the expected relationship with java/lang/Throwable */
			Trc_RTV_validateClassRelationships_InvalidRelationship(vmThread, J9RELATIONSHIP_JAVA_LANG_THROWABLE_STRING_LENGTH, J9RELATIONSHIP_JAVA_LANG_THROWABLE_STRING);
			failedClass = parentClass;
			goto validateDone;
		}
	}

	parentClassNode = J9_LINKED_LIST_START_DO(classRelationshipEntry->root);

	while (NULL != parentClassNode) {
		/* Find the parent class in the loaded classes table */
		parentClass = J9_VM_FUNCTION(vmThread, hashClassTableAt)(classLoader, parentClassNode->className, parentClassNode->classNameLength);

		/* If the parent class has not been loaded, then it has to be an interface since the child class is already loaded */
		if (NULL == parentClass) {
			/* Add a new relationship to the table if one doesn't already exist and flag the parentClass as J9RELATIONSHIP_MUST_BE_INTERFACE */
			J9ClassRelationship *parentClassEntry = findClassRelationship(vmThread, classLoader, parentClassNode->className, parentClassNode->classNameLength);

			Trc_RTV_validateClassRelationships_ParentClassNotLoaded(vmThread, parentClassNode->classNameLength, parentClassNode->className, parentClassNode);

			if (NULL == parentClassEntry) {
				J9ClassRelationship classRelationship = {0};
				classRelationship.className = (U_8 *) j9mem_allocate_memory(parentClassNode->classNameLength + 1, J9MEM_CATEGORY_CLASSES_CRV_RELATIONSHIPS);

				/* className for parent class successfully allocated, continue initialization of parent class entry */
				if (NULL != classRelationship.className) {
					Trc_RTV_validateClassRelationships_AllocatingParentClass(vmThread);
					memcpy(classRelationship.className, parentClassNode->className, parentClassNode->classNameLength);
					classRelationship.className[parentClassNode->classNameLength] = '\0';
					classRelationship.classNameLength = parentClassNode->classNameLength;
					classRelationship.flags = J9RELATIONSHIP_MUST_BE_INTERFACE;

					parentClassEntry = hashTableAdd(classLoader->classRelationshipsHashTable, &classRelationship);

					if (NULL == parentClassEntry) {
						Trc_RTV_classRelationships_ParentAllocationFailed(vmThread);
						j9mem_free_memory(classRelationship.className);
						failedClass = childClass;
						goto validateDone;
					}
					Trc_RTV_validateClassRelationships_AllocatedParentClassEntry(vmThread);
				} else {
					Trc_RTV_classRelationships_ParentAllocationFailed(vmThread);
					failedClass = childClass;
					goto validateDone;
				}
			} else {
				parentClassEntry->flags |= J9RELATIONSHIP_MUST_BE_INTERFACE;
			}
		} else {
			/* The already loaded parentClass should either be an interface, or is the same or superclass of the childClass */
			if (J9ROMCLASS_IS_INTERFACE(parentClass->romClass)) {
				/* If the parent is an interface, be permissive as per the verifier type checking rules */
				Trc_RTV_validateClassRelationships_ParentClassIsInterface(vmThread, parentClassNode->classNameLength, parentClassNode->className, parentClassNode);
			} else if (isSameOrSuperClassOf(parentClass, childClass)) {
				Trc_RTV_validateClassRelationships_ParentClassIsSuperClass(vmThread, parentClassNode->classNameLength, parentClassNode->className, parentClassNode);
			} else {
				/* The child class and parent class have an invalid relationship */
				Trc_RTV_validateClassRelationships_InvalidRelationship(vmThread, parentClassNode->classNameLength, parentClassNode->className);
				failedClass = parentClass;
				goto validateDone;
			}
		}
		parentClassNode = J9_LINKED_LIST_NEXT_DO(classRelationshipEntry->root, parentClassNode);
	}

	/* Successful validation; free memory for classRelationshipEntry */
	freeClassRelationshipNodes(vmThread, classLoader, classRelationshipEntry);
	j9mem_free_memory(classRelationshipEntry->className);
	hashTableRemove(classLoader->classRelationshipsHashTable, classRelationshipEntry);

validateDone:
	Trc_RTV_validateClassRelationships_Exit(vmThread, failedClass);
	return failedClass;
}

/**
 * Add a parentClassNode to a child class entry's linked list of parent classes.
 *
 * Return the allocated J9ClassRelationshipNode.
 */
static J9ClassRelationshipNode *
allocateClassRelationshipNode(J9VMThread *vmThread, J9ClassLoader *classLoader, U_8 *className, UDATA classNameLength)
{
	PORT_ACCESS_FROM_VMC(vmThread);
	J9ClassRelationshipNode *parentClassNode = (J9ClassRelationshipNode *) pool_newElement(classLoader->classRelationshipsPool);

	if (NULL != parentClassNode) {
		parentClassNode->className = (U_8 *) j9mem_allocate_memory(classNameLength + 1, J9MEM_CATEGORY_CLASSES_CRV_RELATIONSHIPS);

		if (NULL != parentClassNode->className) {
			memcpy(parentClassNode->className, className, classNameLength);
			parentClassNode->className[classNameLength] = '\0';
			parentClassNode->classNameLength = classNameLength;
		} else {
			pool_removeElement(classLoader->classRelationshipsPool, parentClassNode);
			parentClassNode = NULL;
		}
	}

	return parentClassNode;
}

/**
 * Find the class relationship table entry for a particular class.
 *
 * Returns the found J9ClassRelationship, or NULL if it is not found.
 */
static J9ClassRelationship *
findClassRelationship(J9VMThread *vmThread, J9ClassLoader *classLoader, U_8 *className, UDATA classNameLength)
{
	J9ClassRelationship *classEntry = NULL;

	Trc_RTV_findClassRelationship_Entry(vmThread, classNameLength, className);

	if (NULL != classLoader->classRelationshipsHashTable) {
		J9ClassRelationship exemplar = {0};
		exemplar.className = className;
		exemplar.classNameLength = classNameLength;
		classEntry = hashTableFind(classLoader->classRelationshipsHashTable, &exemplar);
	}

	Trc_RTV_findClassRelationship_Exit(vmThread, classEntry);
	return classEntry;
}

/**
 * Free allocated memory for each parent class node of a class relationship table entry.
 */
static void
freeClassRelationshipNodes(J9VMThread *vmThread, J9ClassLoader *classLoader, J9ClassRelationship *relationship)
{
	PORT_ACCESS_FROM_VMC(vmThread);
	J9ClassRelationshipNode *parentClassNode = NULL;

	Trc_RTV_freeClassRelationshipNodes_Entry(vmThread, relationship->classNameLength, relationship->className);

	while (NULL != relationship->root) {
		parentClassNode = relationship->root;
		Trc_RTV_freeClassRelationshipNodes_ParentClass(vmThread, parentClassNode->classNameLength, parentClassNode->className);
		J9_LINKED_LIST_REMOVE(relationship->root, parentClassNode);
		j9mem_free_memory(parentClassNode->className);
		pool_removeElement(classLoader->classRelationshipsPool, parentClassNode);
	}

	Trc_RTV_freeClassRelationshipNodes_Exit(vmThread);
	return;
}

/**
 * Allocates new hash table to store class relationship entries
 * and new pool to store class relationship nodes.
 *
 * Returns 0 if successful, and 1 otherwise.
 */
static UDATA
allocateClassRelationshipTableAndPool(J9ClassLoader *classLoader, J9JavaVM *vm)
{
	UDATA result = 0;

	classLoader->classRelationshipsHashTable = hashTableNew(OMRPORT_FROM_J9PORT(vm->portLibrary), J9_GET_CALLSITE(), 256, sizeof(J9ClassRelationship), sizeof(char *), 0, J9MEM_CATEGORY_CLASSES_CRV_RELATIONSHIPS, relationshipHashFn, relationshipHashEqualFn, NULL, vm);

	if (NULL == classLoader->classRelationshipsHashTable) {
		result = 1;
	} else {
		UDATA minNumElements = J9RELATIONSHIP_NODE_COUNT_MINIMUM;

		if (vm->systemClassLoader == classLoader) {
			minNumElements = J9RELATIONSHIP_NODE_COUNT_MINIMUM_SYSTEM_CLASSLOADER;
		} else if (vm->extensionClassLoader == classLoader) {
			minNumElements = J9RELATIONSHIP_NODE_COUNT_MINIMUM_EXTENSION_CLASSLOADER;
		} else if (vm->applicationClassLoader == classLoader) {
			minNumElements = J9RELATIONSHIP_NODE_COUNT_MINIMUM_APPLICATION_CLASSLOADER;
		}

		classLoader->classRelationshipsPool = pool_new(sizeof(J9ClassRelationshipNode), minNumElements, 0, 0, J9_GET_CALLSITE(), J9MEM_CATEGORY_CLASSES_CRV_RELATIONSHIPS, POOL_FOR_PORT(vm->portLibrary));

		if (NULL == classLoader->classRelationshipsPool) {
			result = 1;
		}
	}

	return result;
}

/**
 * Frees memory for each J9ClassRelationship table entry and J9ClassRelationshipNode.
 */
void
j9bcv_freeClassRelationshipTableAndPool(J9VMThread *vmThread, J9ClassLoader *classLoader)
{
	if (NULL != classLoader->classRelationshipsHashTable) {
		PORT_ACCESS_FROM_VMC(vmThread);
		J9HashTableState hashTableState = {0};
		J9ClassRelationship *relationshipEntryStart = (J9ClassRelationship *) hashTableStartDo(classLoader->classRelationshipsHashTable, &hashTableState);
		J9ClassRelationship *relationshipEntry = relationshipEntryStart;

		/* Free all parent class nodes of a relationship entry and then free the entry itself */
		while (NULL != relationshipEntry) {
			UDATA result = 0;
			freeClassRelationshipNodes(vmThread, classLoader, relationshipEntry);
			j9mem_free_memory(relationshipEntry->className);
			result = hashTableDoRemove(&hashTableState);
			Assert_RTV_true(0 == result);
			relationshipEntry = (J9ClassRelationship *) hashTableNextDo(&hashTableState);
		}

		hashTableFree(classLoader->classRelationshipsHashTable);
		classLoader->classRelationshipsHashTable = NULL;

		pool_kill(classLoader->classRelationshipsPool);
		classLoader->classRelationshipsPool = NULL;
	}
	return;
}

/**
 * Hash function for J9ClassRelationship entries.
 */
static UDATA
relationshipHashFn(void *key, void *userData)
{
	J9ClassRelationship *relationshipKey = key;
	J9JavaVM *vm = userData;

	UDATA utf8HashClass = J9_VM_FUNCTION_VIA_JAVAVM(vm, computeHashForUTF8)(relationshipKey->className, relationshipKey->classNameLength);

	return utf8HashClass;
}

/**
 * Hash equal function for J9ClassRelationship entries.
 */
static UDATA
relationshipHashEqualFn(void *leftKey, void *rightKey, void *userData)
{
	J9ClassRelationship *left_relationshipKey = leftKey;
	J9ClassRelationship *right_relationshipKey = rightKey;

	UDATA classNameEqual = J9UTF8_DATA_EQUALS(left_relationshipKey->className, left_relationshipKey->classNameLength, right_relationshipKey->className, right_relationshipKey->classNameLength);

	return classNameEqual;
}
