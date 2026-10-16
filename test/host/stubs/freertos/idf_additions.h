#pragma once
typedef void (*TaskFunction_t)(void *);
static inline int xTaskCreateWithCaps(TaskFunction_t pxTaskCode,
                            const char * const pcName,
                            const unsigned int ulStackDepth,
                            void * const pvParameters,
                            unsigned int uxPriority,
                            void * const pxCreatedTask,
                            int caps) {
    (void)pcName; (void)ulStackDepth; (void)uxPriority; (void)pxCreatedTask; (void)caps;
    pxTaskCode(pvParameters);
    return 1;
}
