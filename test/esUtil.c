#include "esUtil.h"
#include <string.h>
#include <math.h>

void esMatrixLoadIdentity(ESMatrix *result)
{
    memset(result, 0, sizeof(ESMatrix));
    result->m[0][0] = 1.0f;
    result->m[1][1] = 1.0f;
    result->m[2][2] = 1.0f;
    result->m[3][3] = 1.0f;
}

void esMatrixMultiply(ESMatrix *result, ESMatrix *srcA, ESMatrix *srcB)
{
    ESMatrix tmp;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp.m[i][j] = 0.0f;
            for (int k = 0; k < 4; k++)
                tmp.m[i][j] += srcA->m[i][k] * srcB->m[k][j];
        }
    }
    memcpy(result, &tmp, sizeof(ESMatrix));
}

void esTranslate(ESMatrix *result, GLfloat tx, GLfloat ty, GLfloat tz)
{
    result->m[3][0] += result->m[0][0] * tx + result->m[1][0] * ty + result->m[2][0] * tz;
    result->m[3][1] += result->m[0][1] * tx + result->m[1][1] * ty + result->m[2][1] * tz;
    result->m[3][2] += result->m[0][2] * tx + result->m[1][2] * ty + result->m[2][2] * tz;
    result->m[3][3] += result->m[0][3] * tx + result->m[1][3] * ty + result->m[2][3] * tz;
}

void esRotate(ESMatrix *result, GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat rad = angle * 3.14159265f / 180.0f;
    GLfloat s = sinf(rad);
    GLfloat c = cosf(rad);
    GLfloat len = sqrtf(x*x + y*y + z*z);
    if (len != 0.0f) { x /= len; y /= len; z /= len; }

    ESMatrix r;
    esMatrixLoadIdentity(&r);
    r.m[0][0] = x*x*(1-c)+c;   r.m[0][1] = y*x*(1-c)+z*s; r.m[0][2] = x*z*(1-c)-y*s;
    r.m[1][0] = x*y*(1-c)-z*s; r.m[1][1] = y*y*(1-c)+c;   r.m[1][2] = y*z*(1-c)+x*s;
    r.m[2][0] = x*z*(1-c)+y*s; r.m[2][1] = y*z*(1-c)-x*s; r.m[2][2] = z*z*(1-c)+c;

    ESMatrix tmp;
    memcpy(&tmp, result, sizeof(ESMatrix));
    esMatrixMultiply(result, &tmp, &r);
}

void esFrustum(ESMatrix *result, float left, float right,
               float bottom, float top, float nearZ, float farZ)
{
    float deltaX = right - left;
    float deltaY = top - bottom;
    float deltaZ = farZ - nearZ;

    memset(result, 0, sizeof(ESMatrix));
    result->m[0][0] = 2.0f * nearZ / deltaX;
    result->m[1][1] = 2.0f * nearZ / deltaY;
    result->m[2][0] = (right + left) / deltaX;
    result->m[2][1] = (top + bottom) / deltaY;
    result->m[2][2] = -(nearZ + farZ) / deltaZ;
    result->m[2][3] = -1.0f;
    result->m[3][2] = -2.0f * nearZ * farZ / deltaZ;
}
