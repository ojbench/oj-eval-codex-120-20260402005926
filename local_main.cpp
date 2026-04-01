#include <bits/stdc++.h>
#include "src.hpp"
#include "Task.hpp"

int main() {
    Timer timer;    
    Task task0("task0", 2, 3), task1("task1", 10, 9), task2("task2", 8, 8);
    timer.tick();
    timer.addTask(&task1);
    
    timer.addTask(&task2);
    timer.tick();
    timer.addTask(&task0);
    for (int i = 3; i < 31; ++i) {
        auto tasks = timer.tick();
        if (!tasks.empty()) {
            std::cout << "tick " << i << std::endl;
            for (auto task : tasks) {
                task->execute();
            }
        }
    }
    return 0;
}
