#ifdef MATH_FAST
int tabdata_value(void);
int math_value(void) { return tabdata_value() + 1; }
#else
int math_value(void) { return 1; }
#endif
