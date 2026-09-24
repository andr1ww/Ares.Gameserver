#pragma once

// ticks reenter through the PE hook, game thread only
struct FReentrancyGuard
{
    explicit FReentrancyGuard(bool& InFlag)
        : Flag(InFlag)
        , bEntered(!InFlag)
    {
        if (bEntered)
            Flag = true;
    }

    ~FReentrancyGuard()
    {
        if (bEntered)
            Flag = false;
    }

    bool Entered() const { return bEntered; }

private:
    bool& Flag;
    bool bEntered;
};
