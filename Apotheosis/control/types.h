#pragma once

#include <cstdint>

namespace control {

struct Vec2
{
    double x = 0.0;
    double y = 0.0;

    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& o) const { return { x + o.x, y + o.y }; }
    Vec2 operator-(const Vec2& o) const { return { x - o.x, y - o.y }; }
    Vec2 operator*(double s) const { return { x * s, y * s }; }
    Vec2 operator/(double s) const { return { x / s, y / s }; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }

    double dot(const Vec2& o) const { return x * o.x + y * o.y; }
    double norm() const;
    double normSq() const { return x * x + y * y; }
};

struct Box
{
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    double centerX() const { return x + w * 0.5; }
    double centerY() const { return y + h * 0.5; }
    Vec2 center() const { return { centerX(), centerY() }; }

    double area() const { return w * h; }
    double diagonal() const;

    bool valid() const { return w > 0.0 && h > 0.0; }
};

struct Candidate
{
    Box box;
    int classId = -1;
    double confidence = 0.0;
};

struct Counts
{
    int x = 0;
    int y = 0;
};

enum class Axis { X = 0, Y = 1 };

inline Vec2 axisVec(Axis a, double v)
{
    return (a == Axis::X) ? Vec2{ v, 0.0 } : Vec2{ 0.0, v };
}

}
