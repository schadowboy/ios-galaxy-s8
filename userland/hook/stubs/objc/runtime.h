/* Host syntax-check stub (NOT used for the iOS build): just the declarations eagl_s8.c needs from <objc/runtime.h>. */
#ifndef STUB_OBJC_RUNTIME_H
#define STUB_OBJC_RUNTIME_H
#include <stdbool.h>
typedef struct objc_object *id;
typedef struct objc_class *Class;
typedef struct objc_selector *SEL;
typedef struct objc_method *Method;
typedef void (*IMP)(void);
typedef bool BOOL;
#define YES true
#define NO false
#define nil ((id)0)
#define Nil ((Class)0)
struct objc_super {
	id receiver;
	Class super_class;
};
Class objc_getClass(const char *name);
SEL sel_registerName(const char *name);
Method class_getInstanceMethod(Class cls, SEL name);
Method class_getClassMethod(Class cls, SEL name);
IMP method_getImplementation(Method m);
const char *method_getTypeEncoding(Method m);
IMP class_replaceMethod(Class cls, SEL name, IMP imp, const char *types);
Class class_getSuperclass(Class cls);
Class object_getClass(id obj);
#endif
