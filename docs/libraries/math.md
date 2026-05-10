# `math`

The `math` namespace provides constants, basic numeric utilities,
trigonometry, exponentials, and a pseudo-random number generator.

All angles are in **radians**.

## Constants

| Name        | Value             | Notes                                     |
|-------------|-------------------|-------------------------------------------|
| `math.pi`   | π ≈ 3.14159…      |                                           |
| `math.tau`  | 2π ≈ 6.28318…     | One full turn — often more useful than π. |
| `math.e`    | e ≈ 2.71828…      | Euler's number.                           |
| `math.huge` | +∞                | IEEE-754 positive infinity.               |

```cdo
print(math.pi);             // 3.141592653589793
print(math.huge);           // inf
print(math.huge == math.huge);   // true
print(1 / 0 == math.huge);  // true
```

## General-purpose

### `math.abs(x) → number`

Absolute value.

```cdo
print(math.abs(-7));        // 7
print(math.abs(3.5));       // 3.5
```

### `math.sign(x) → number`

`-1` for negative, `0` for zero, `1` for positive.

```cdo
print(math.sign(-5));       // -1
print(math.sign(0));        // 0
print(math.sign(99));       // 1
```

### `math.min(...)`, `math.max(...)`

Smallest / largest of the arguments.

```cdo
print(math.min(3, 1, 4, 1, 5, 9, 2, 6));   // 1
print(math.max(3, 1, 4, 1, 5, 9, 2, 6));   // 9
```

### `math.clamp(v, low, high) → number`

Constrain `v` to `[low, high]`.

```cdo
print(math.clamp(150, 0, 100));     // 100
print(math.clamp(-5, 0, 100));      // 0
print(math.clamp(42, 0, 100));      // 42
```

## Rounding

### `math.floor(x)`, `math.ceil(x)`, `math.round(x)`

Standard IEEE rounding.  Each returns a number; the result is still a
double, even though it is integer-valued.

```cdo
print(math.floor(3.7));     // 3
print(math.ceil(3.2));      // 4
print(math.round(3.5));     // 4
print(math.round(2.5));     // 3   — banker's rounding (round half to even)
```

## Powers and logs

### `math.sqrt(x)`, `math.pow(x, y)`

Square root and `x` raised to `y`.  `pow` is the function form; the
operator form is `x ^ y`.

```cdo
print(math.sqrt(16));       // 4
print(math.pow(2, 10));     // 1024
print(2 ^ 10);              // 1024 — same thing
```

### `math.exp(x)`, `math.log(x)`, `math.log(x, base)`, `math.log10(x)`

Natural exponential, natural log, log to a given base, log base 10.

```cdo
print(math.exp(1));         // 2.718281828459045  (== math.e)
print(math.log(math.e));    // 1
print(math.log(8, 2));      // 3
print(math.log10(1000));    // 3
```

## Trigonometry

All angles are in radians.

### `math.sin(x)`, `math.cos(x)`, `math.tan(x)`

```cdo
print(math.sin(0));               // 0
print(math.cos(math.pi));         // -1
print(math.tan(math.pi / 4));     // 1
```

### `math.asin(x)`, `math.acos(x)`, `math.atan(x)`, `math.atan2(y, x)`

Inverse trig.  `atan2(y, x)` returns the angle of the vector `(x, y)`
in the range `(-π, π]`, handling the quadrant correctly.

```cdo
print(math.atan2(1, 0));          // ≈ 1.5708  (π/2)
print(math.atan2(-1, -1));        // ≈ -2.356  (-3π/4)
```

### `math.sinh(x)`, `math.cosh(x)`

Hyperbolic sine and cosine.

### `math.rad(deg)`, `math.deg(rad)`

Angle conversion between degrees and radians.

```cdo
print(math.rad(180));             // 3.141592653589793
print(math.deg(math.pi));         // 180
```

## Random

### `math.random()`, `math.random(max)`, `math.random(min, max)`

- `math.random()` — random `f64` in `[0, 1)`.
- `math.random(max)` — random integer in `[0, max)`.
- `math.random(min, max)` — random integer in `[min, max)`.

`math.random` uses `rand()` seeded from `time(NULL)` on first use.  It
is **not cryptographically secure** — for that, generate bytes via
`crypto.sha256` of an entropy source, or call into the OS RNG via the
embedding API.

```cdo
print(math.random());             // 0.42…  (different each run)
print(math.random(10));           // 0..9
print(math.random(1, 100));       // 1..99
```

## Examples

### Distance between two points

```cdo
FUNCTION distance(x1, y1, x2, y2) {
    VAR dx = x2 - x1;
    VAR dy = y2 - y1;
    RETURN math.sqrt(dx * dx + dy * dy);
}

print(distance(0, 0, 3, 4));      // 5
```

### Map a value from one range to another

```cdo
FUNCTION map_range(v, in_lo, in_hi, out_lo, out_hi) {
    VAR t = (v - in_lo) / (in_hi - in_lo);
    RETURN out_lo + t * (out_hi - out_lo);
}

print(map_range(0.5, 0, 1, 100, 200));    // 150
```

### Sampling a unit circle

```cdo
VAR points = [];
FOR i IN 1 -> 12 {
    VAR a = (i / 12) * math.tau;
    points:push([math.cos(a), math.sin(a)]);
}
print(inspect(points));
```
