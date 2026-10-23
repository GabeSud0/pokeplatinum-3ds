"""The little matrix maths the banner needs, in plain Python (no numpy, so the
build runs on any Python 3.12, MSYS2's included).

Matrices are lists of rows; 4x4 ones are affine, column-vector convention
(p' = M p). Vectors are lists.
"""
import math


def eye(n=4):
    return [[1.0 if i == j else 0.0 for j in range(n)] for i in range(n)]


def mul(a, b):
    """a @ b (matrix x matrix)."""
    n, m, p = len(a), len(b), len(b[0])
    return [[sum(a[i][k] * b[k][j] for k in range(m)) for j in range(p)] for i in range(n)]


def apply(m, v):
    """m @ v (matrix x vector)."""
    return [sum(m[i][k] * v[k] for k in range(len(v))) for i in range(len(m))]


def point(m, p):
    """The affine 4x4 m applied to the 3D point p."""
    return apply(m, [p[0], p[1], p[2], 1.0])[:3]


def upper3(m):
    return [row[:3] for row in m[:3]]


def transpose(m):
    return [list(r) for r in zip(*m)]


def inverse(m):
    """Gauss-Jordan with partial pivoting (any size)."""
    n = len(m)
    a = [list(map(float, row)) + [1.0 if i == j else 0.0 for j in range(n)] for i, row in enumerate(m)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        if abs(a[p][c]) < 1e-12:
            raise ValueError("singular matrix")
        a[c], a[p] = a[p], a[c]
        piv = a[c][c]
        a[c] = [x / piv for x in a[c]]
        for r in range(n):
            if r != c and a[r][c]:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [row[n:] for row in a]


def solve(m, v):
    return apply(inverse(m), v)


def diag(values):
    n = len(values)
    return [[values[i] if i == j else 0.0 for j in range(n)] for i in range(n)]


def cols3(*v):
    """3x3 from 9 numbers in column-major order (how apicula and cgmath list them)."""
    return [[v[0], v[3], v[6]], [v[1], v[4], v[7]], [v[2], v[5], v[8]]]


def trs(t=None, r=None, s=None):
    m = eye()
    if s is not None:
        m = diag([s[0], s[1], s[2], 1.0])
    if r is not None:
        r4 = eye()
        for i in range(3):
            r4[i][:3] = r[i][:]
        m = mul(r4, m)
    if t is not None:
        t4 = eye()
        for i in range(3):
            t4[i][3] = t[i]
        m = mul(t4, m)
    return m


def scale4(s):
    return diag([s, s, s, 1.0])


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def norm(a):
    return math.sqrt(dot(a, a))


def normalize(a):
    n = norm(a)
    return [x / n for x in a] if n else list(a)


def close(a, b, atol=1e-8, rtol=1e-5):
    """numpy.allclose for nested lists: |a - b| <= atol + rtol * |b| everywhere."""
    if isinstance(a, (list, tuple)):
        return all(close(x, y, atol, rtol) for x, y in zip(a, b))
    return abs(a - b) <= atol + rtol * abs(b)


def quat_from_matrix(r):
    """Unit quaternion [x, y, z, w] of a 3x3 rotation."""
    tr = r[0][0] + r[1][1] + r[2][2]
    w = math.sqrt(max(0.0, 1 + tr)) / 2
    if w > 1e-6:
        return [(r[2][1] - r[1][2]) / (4 * w), (r[0][2] - r[2][0]) / (4 * w), (r[1][0] - r[0][1]) / (4 * w), w]
    i = max(range(3), key=lambda k: r[k][k])
    j, k = (i + 1) % 3, (i + 2) % 3
    s = math.sqrt(max(0.0, 1 + r[i][i] - r[j][j] - r[k][k]))
    q = [0.0, 0.0, 0.0, 0.0]
    q[i] = s / 2
    q[j] = (r[j][i] + r[i][j]) / (2 * s)
    q[k] = (r[k][i] + r[i][k]) / (2 * s)
    q[3] = (r[k][j] - r[j][k]) / (2 * s)
    return q


def quat_matrix(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]


def det3(a):
    return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
            + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))


def nearest_rotation(a):
    """The rotation closest to the 3x3 a (its polar factor, U V^T of its SVD),
    by Newton's iteration R <- (R + R^-T) / 2."""
    r = [row[:] for row in a]
    if det3(r) < 0:
        r = [[-x for x in row] for row in r]
    for _ in range(100):
        it = transpose(inverse(r))
        nxt = [[(r[i][j] + it[i][j]) / 2 for j in range(3)] for i in range(3)]
        done = close(nxt, r, atol=1e-15, rtol=0)
        r = nxt
        if done:
            break
    return r


def decompose(m):
    """Translation, rotation quaternion and per-axis scale of an affine 4x4 that
    is rotation x scale, as the joints of these models are: the rotation is the
    nearest one to the matrix (fixed-point matrices are never exactly square)."""
    t = [m[0][3], m[1][3], m[2][3]]
    a = upper3(m)
    r = nearest_rotation(a)
    s_m = mul(transpose(r), a)
    return t, quat_from_matrix(r), [s_m[0][0], s_m[1][1], s_m[2][2]]
