#ifndef IR_MEL_EXPORT_H
#define IR_MEL_EXPORT_H

#ifdef IR_MEL_STATIC_DEFINE
#define IR_MEL_EXPORT
#define IR_MEL_NO_EXPORT
#else
#ifndef IR_MEL_EXPORT
#ifdef CommonIR_MEL_RefImpl_EXPORTS
/* We are building this library */
#define IR_MEL_EXPORT __attribute__((visibility("default")))
#else
/* We are using this library */
#define IR_MEL_EXPORT __attribute__((visibility("default")))
#endif
#endif

#ifndef IR_MEL_NO_EXPORT
#define IR_MEL_NO_EXPORT __attribute__((visibility("hidden")))
#endif
#endif

#ifndef IR_MEL_DEPRECATED
#define IR_MEL_DEPRECATED __attribute__((__deprecated__))
#endif

#ifndef IR_MEL_DEPRECATED_EXPORT
#define IR_MEL_DEPRECATED_EXPORT IR_MEL_EXPORT IR_MEL_DEPRECATED
#endif

#ifndef IR_MEL_DEPRECATED_NO_EXPORT
#define IR_MEL_DEPRECATED_NO_EXPORT IR_MEL_NO_EXPORT IR_MEL_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#ifndef IR_MEL_NO_DEPRECATED
#define IR_MEL_NO_DEPRECATED
#endif
#endif

#endif /* IR_MEL_EXPORT_H */
