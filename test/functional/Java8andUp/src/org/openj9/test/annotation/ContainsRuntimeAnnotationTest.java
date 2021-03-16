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
import java.lang.annotation.ElementType;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

@Test(groups = { "level.sanity" })
public class ContainsRuntimeAnnotationTest {
	@MyFieldAnnotation
	static int myField;

	@MyMethodAnnotation
	static void myMethod() {
		return;
	}

	public ContainsRuntimeAnnotationTest() {}

	private static native boolean fieldContainsRuntimeAnnotation(Field field, String annotationName);
	// private static native boolean methodContainsRuntimeAnnotation(Method method, String annotationName);

	@Test
	public void test_field_annotation() throws Exception {
		Field field = this.getClass().getDeclaredField("myField"); //$NON-NLS-1$
		String annotationName = "LMyFieldAnnotation;"; //$NON-NLS-1$
		boolean result = fieldContainsRuntimeAnnotation(field, annotationName);

		AssertJUnit.assertTrue(result);
	}

	// @Test
	// public void test_method_annotation() throws Exception {
	// 	Method method = this.getClass().getDeclaredMethod("myMethod"); //$NON-NLS-1$
	// 	String annotationName = "LMyMethodAnnotation;"; //$NON-NLS-1$
	// 	boolean result = methodContainsRuntimeAnnotation(method, annotationName);

	// 	AssertJUnit.assertTrue(result);
	// }
}

@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.FIELD)
@interface MyFieldAnnotation {
}

@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.METHOD)
@interface MyMethodAnnotation {
}
