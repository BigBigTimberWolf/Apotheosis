#pragma once

#include "types.h"

namespace control {

class IFilter
{
public:
    virtual ~IFilter() = default;

    virtual void observe(const Vec2& center, double dtSeconds) = 0;

    virtual Vec2 position() const = 0;

    virtual Vec2 velocity() const = 0;

    virtual bool initialized() const = 0;

    virtual void reset() = 0;
};

}
