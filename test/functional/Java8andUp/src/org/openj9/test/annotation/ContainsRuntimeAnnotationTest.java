package org.openj9.test.annotation;

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

import org.testng.Assert;
import org.testng.AssertJUnit;
import org.testng.annotations.Test;
import org.testng.log4testng.Logger;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;
import java.lang.annotation.Annotation;
import java.lang.annotation.ElementType;

import java.util.Arrays;

import jdk.internal.misc.SharedSecrets;
import jdk.internal.reflect.ConstantPool;

@Test(groups = { "level.sanity" })
public class ContainsRuntimeAnnotationTest {
	private static final String THIS_CLASS_NAME = "org/openj9/test/annotation/ContainsRuntimeAnnotationTest"; //$NON-NLS-1$
	private final ConstantPool constantPool;

	static {
		try {
			System.loadLibrary("annotationtests"); //$NON-NLS-1$
		} catch (UnsatisfiedLinkError e) {
			Assert.fail(e.getMessage() + "\nlibrary path = " + System.getProperty("java.library.path")); //$NON-NLS-1$ //$NON-NLS-2$
		}
	}

	@MyFieldAnnotation
	int myField = 0;

	@MyMethodAnnotation
	void myMethod() {}

	public ContainsRuntimeAnnotationTest() {
		constantPool = SharedSecrets.getJavaLangAccess().getConstantPool(this.getClass());
	}

	private static native boolean containsRuntimeAnnotation(int cpIndex, String annotationName, boolean isField);

	@Test
	public void test_field_annotation() throws Exception {
		boolean annotationFound = false;
		int cpIndex = getMemberCPIndex("myField", "I", true); //$NON-NLS-1$ //$NON-NLS-2$

		if (-1 != cpIndex) {
			String annotationName = "MyFieldAnnotation"; //$NON-NLS-1$
			annotationFound = containsRuntimeAnnotation(cpIndex, annotationName, true);
		}

		AssertJUnit.assertTrue(annotationFound);
	}

	@Test
	public void test_method_annotation() throws Exception {
		boolean annotationFound = false;

		// Call the method so it shows up in the constant pool
		myMethod();

		int cpIndex = getMemberCPIndex("myMethod", "()V", false); //$NON-NLS-1$ //$NON-NLS-2$

		if (-1 != cpIndex) {
			String annotationName = "MyMethodAnnotation"; //$NON-NLS-1$
			annotationFound = containsRuntimeAnnotation(cpIndex, annotationName, false);
		}

		AssertJUnit.assertTrue(annotationFound);
	}

	private int getMemberCPIndex(String memberName, String memberType, boolean isField) {
		int cpIndex = -1;

		for (int i = constantPool.getSize() - 1; i >= 0; i--) {
			try {
				if (isField) {
					// If this doesn't fail, then the item at index i in the constant pool is a field
					constantPool.getFieldAt(i);
				} else {
					// Assumes the member is a method
					// If this doesn't fail, then the item at index i in the constant pool is a method
					constantPool.getMethodAt(i);
				}

				// Returns 3-element array of class name, member name and type
				String [] cpMemberInfo = constantPool.getMemberRefInfoAt(i);

				if (THIS_CLASS_NAME.equals(cpMemberInfo[0])
					&& memberName.equals(cpMemberInfo[1])
					&& memberType.equals(cpMemberInfo[2])
				) {
					cpIndex = i;
					break;
				}
			} catch (Throwable ignored) {
				// Ignore errors if the constant pool entry doesn't exist
			}
		}

		return cpIndex;
	}
}

@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.FIELD)
@interface MyFieldAnnotation {}

@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.METHOD)
@interface MyMethodAnnotation {}
