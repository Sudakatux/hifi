//
//  glewand.c
//  libraries/gl/src/gl
//
//  Created by Cristian Duarte & Gabriel Calero on 10/3/16.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "glewand.h"
#include <EGL/egl.h>

#define glewGetProcAddress(name) eglGetProcAddress(name)

PFNGLQUERYCOUNTEREXTPROC __glewQueryCounterEXT = NULL;
PFNGLGETQUERYOBJECTUI64VEXTPROC __glewGetQueryObjectui64vEXT = NULL;

GLenum GLEWAPIENTRY glewInit (void)
{
  GLboolean r = GL_FALSE;

  r = ((glQueryCounterEXT = (PFNGLQUERYCOUNTEREXTPROC)glewGetProcAddress("glQueryCounterEXT")) == NULL) || r;
  r = ((glGetQueryObjectui64vEXT = (PFNGLGETQUERYOBJECTUI64VEXTPROC)glewGetProcAddress("glGetQueryObjectui64vEXT")) == NULL) || r;

  //return r;
  return GLEW_OK; // Manage cases
}