// sal_disable.h - forces SAL annotations off for legacy v100 compiler
#pragma once
#ifndef _USE_DECLSPECS_FOR_SAL
#define _USE_DECLSPECS_FOR_SAL 0
#endif
#ifndef _USE_ATTRIBUTES_FOR_SAL
#define _USE_ATTRIBUTES_FOR_SAL 0
#endif
#ifndef __ANNOTATION
#define __ANNOTATION(fun)
#endif
#ifndef __PRIMOP
#define __PRIMOP(type, fun)
#endif
#ifndef __QUALIFIER
#define __QUALIFIER(type, fun)
#endif
