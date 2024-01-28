#include "types.h"
#include "stat.h"
#include "user.h"

void thread_function(void *arg1, void *arg2) {
    printf(1, "In child thread with arg1: %d, arg2: %d\n", *(int*)arg1, *(int*)arg2);
    exit();
}

int main() {
    int arg1 = 10, arg2 = 20;
    void *stack = malloc(4096);

    int thread_id = clone(thread_function, &arg1, &arg2, stack);
    if(thread_id < 0) {
        printf(1, "Error creating thread\n");
    } else {
        printf(1, "Created thread with id: %d\n", thread_id);
    }

    wait(); // Wait for the child thread to finish
    free(stack);
    exit();
    ;
}
