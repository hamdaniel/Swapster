#include <windows.h>

#include <iostream>

/**
 * Swapster client: signals the Swapster server's named event.
 *
 * Run the server first so the event exists.
 */
int main() {
    HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Global\\SwapsterTriggerEvent");
    if (!hEvent) {
        std::cerr << "Failed to open trigger event. Is the server running?" << std::endl;
        return 1;
    }

    SetEvent(hEvent);
    CloseHandle(hEvent);

    std::cout << "Trigger sent." << std::endl;
    return 0;
}
