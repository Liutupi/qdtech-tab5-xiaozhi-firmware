#pragma once
#include "display.h"
class Board { public: static Board& GetInstance(); Display* GetDisplay(); };
