#include "stereokit.h"

#include <directxmath.h> // Matrix math functions and objects
using namespace DirectX;

inline XMVECTOR to_fast3  (const vec3 &vec) { return XMLoadFloat3((XMFLOAT3 *)& vec); }
inline vec3     from_fast3(const XMVECTOR &a) { vec3 result; XMStoreFloat3((XMFLOAT3 *)& result, a); return result; }
inline quat     from_fastq(const XMVECTOR &a) { quat result; XMStoreFloat4((XMFLOAT4 *)& result, a); return result; }

void transform_initialize(transform_t &transform) {
	transform._position = { 0,0,0 };
	transform._rotation = { 0,0,0,1 };
	transform._scale    = { 1,1,1 };
	transform._dirty   = true;
}
void transform_set(transform_t &transform, const vec3 &position, const vec3 &scale, const quat &rotation) {
	transform._position = position;
	transform._scale    = scale;
	transform._rotation = rotation;
	transform._dirty   = true;
}
void transform_set_pos(transform_t &transform, const vec3 &position) {
	transform._position = position;
	transform._dirty   = true;
}
void transform_set_scale(transform_t &transform, const vec3 &scale) {
	transform._scale    = scale;
	transform._dirty   = true;
}
void transform_set_rot  (transform_t &transform, const quat &rotation) {
	transform._rotation = rotation;
	transform._dirty   = true;
}
vec3 transform_forward  (transform_t &transform) {
	vec3 forward = { 0,0,-1 };
	return transform._rotation * forward;
}
void transform_lookat  (transform_t &transform, const vec3 &at) {
	XMMATRIX mat = XMMatrixLookAtRH(to_fast3(transform._position), to_fast3(at), XMVectorSet(0, 1, 0, 0));
	transform._rotation = from_fastq(XMQuaternionRotationMatrix(XMMatrixTranspose(mat)));
	transform._dirty = true;
}
void transform_matrix(transform_t &transform, XMMATRIX &result) {
	if (transform._dirty) {
		transform._dirty = false;
		transform._transform = XMMatrixAffineTransformation(
			XMLoadFloat3((XMFLOAT3*)&transform._scale), DirectX::g_XMZero,
			XMLoadFloat4((XMFLOAT4*)&transform._rotation),
			XMLoadFloat3((XMFLOAT3*)&transform._position));
	}
	result = transform._transform;
}