// The ABI probe generates no executable and never evaluates ImGui assertions.
#pragma once
#define assert(expression) ((void)0)
