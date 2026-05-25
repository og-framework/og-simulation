// SPDX-License-Identifier: MPL-2.0
// Copyright Epic Games, Inc. All Rights Reserved.

#include "DMathUtil.h"


namespace dMathUtil
{

// void getRotationMatrix(glm::vec3 fromDirection, glm::vec3 toDirection, glm::mat4& outTransform)
// {
// 		const float dot = glm::dot(fromDirection, toDirection);
// 		const bool fromEqualsTo = abs(abs(dot) - 1.f) < 0.0001f;
// 		if (fromEqualsTo)
// 			outTransform = glm::mat4(1.0f);

// 		const float angle = glm::acos(dot);
// 		const glm::vec3 rotationAxis = glm::normalize(glm::cross(fromDirection, toDirection));
// 		outTransform = glm::rotate(glm::mat4(1.f), angle, rotationAxis);
// }

}