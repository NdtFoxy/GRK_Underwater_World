#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
// NOTE: we deliberately use ONLY gtc/quaternion.hpp (angleAxis, quat-vec
// multiply, normalize) and avoid gtx/quaternion.hpp, which would require
// GLM_ENABLE_EXPERIMENTAL and could break the build on some GLM versions.
#include <vector>

// Movement directions (abstracted from the windowing system).
enum Camera_Movement { FORWARD, BACKWARD, LEFT, RIGHT, UP, DOWN };

const float YAW         = -90.0f;
const float PITCH       =  0.0f;
const float SPEED       =  10.0f;
const float SPRINT_MULT =  6.0f;
const float SENSITIVITY =  0.1f;
const float ZOOM        =  50.0f;

// ======================================================================
// ЗАЩИТА (обязательный метод: Quaternion camera control)
//   СУТЬ:     ориентация = glm::quat. Повороты — композиция angleAxis
//             (yaw вокруг МИРОВОЙ оси, pitch вокруг ЛОКАЛЬНОЙ right).
//             Базис (Front/Right/Up) берётся прямо из кватерниона.
//   ПОЧЕМУ:   нет матриц Эйлера => нет gimbal lock, повороты плавные.
//   ГДЕ:      обзор мышью; на «полюсах» (вверх/вниз) не кувыркается.
//   СЛОВА:    "no gimbal lock", glm::quat, angleAxis, soft pitch-clamp.
// ======================================================================
// ----------------------------------------------------------------------
// QUATERNION CAMERA  (mandatory method: quaternion camera control)
//
// Orientation is stored as a glm::quat and updated by composing
// incremental rotations:
//   * yaw   — rotation about the WORLD up axis  (keeps the horizon level)
//   * pitch — rotation about the camera's LOCAL right axis
//
// Because the basis vectors are derived directly from the quaternion
// (never from Euler angle matrices), the camera has NO gimbal lock and
// rotations stay smooth at any orientation. Pitch is softly clamped so
// the view can't flip over the poles.
// ----------------------------------------------------------------------
class Camera {
public:
    glm::vec3 Position;
    glm::vec3 Front;
    glm::vec3 Up;
    glm::vec3 Right;
    glm::vec3 WorldUp;

    // The single source of truth for orientation.
    glm::quat Orientation;

    // Kept only for code that still reads them (e.g. initial setup);
    // they are NOT used to build the rotation.
    float Yaw;
    float Pitch;

    float MovementSpeed;
    float MouseSensitivity;
    float Zoom;

    // Optional view-matrix override (used by the wave-bob system).
    glm::mat4 viewOverride = glm::mat4(1.0f);
    bool      useViewOverride = false;

    Camera(glm::vec3 position = glm::vec3(0.0f),
           glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f),
           float yaw = YAW, float pitch = PITCH)
        : Front(glm::vec3(0.0f, 0.0f, -1.0f)),
          MovementSpeed(SPEED), MouseSensitivity(SENSITIVITY), Zoom(ZOOM) {
        Position = position;
        WorldUp  = glm::normalize(up);
        Yaw = yaw; Pitch = pitch;
        SetEuler(yaw, pitch);
    }

    // Build the orientation quaternion from yaw/pitch (used once at
    // startup so we can aim the camera at a known direction).
    void SetEuler(float yawDeg, float pitchDeg) {
        glm::quat qYaw   = glm::angleAxis(glm::radians(yawDeg + 90.0f),
                                          glm::vec3(0.0f, -1.0f, 0.0f));
        glm::quat qPitch = glm::angleAxis(glm::radians(pitchDeg),
                                          glm::vec3(1.0f, 0.0f, 0.0f));
        Orientation = glm::normalize(qYaw * qPitch);
        updateVectors();
    }

    glm::mat4 GetViewMatrix() {
        if (useViewOverride) return viewOverride;
        return glm::lookAt(Position, Position + Front, Up);
    }
    void SetViewOverride(const glm::mat4& m) { viewOverride = m; useViewOverride = true; }
    void ClearViewOverride() { useViewOverride = false; }

    void ProcessKeyboard(Camera_Movement direction, float deltaTime, bool sprint = false) {
        float velocity = MovementSpeed * deltaTime * (sprint ? SPRINT_MULT : 1.0f);
        if (direction == FORWARD)  Position += Front * velocity;
        if (direction == BACKWARD) Position -= Front * velocity;
        if (direction == LEFT)     Position -= Right * velocity;
        if (direction == RIGHT)    Position += Right * velocity;
        if (direction == UP)       Position += Up * velocity;
        if (direction == DOWN)     Position -= Up * velocity;
    }

    // Mouse look — composed as quaternion rotations (gimbal-lock free).
    void ProcessMouseMovement(float xoffset, float yoffset, bool constrainPitch = true) {
        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;

        // World-space yaw keeps the horizon level regardless of pitch.
        glm::quat qYaw = glm::angleAxis(glm::radians(-xoffset), WorldUp);
        // Local-space pitch about the current right axis.
        glm::quat qPitch = glm::angleAxis(glm::radians(yoffset), Right);

        glm::quat newOri = glm::normalize(qYaw * qPitch * Orientation);

        // Soft pitch clamp: reject the update if it would tip the view
        // past ~89° (prevents the camera flipping over the top/bottom).
        if (constrainPitch) {
            glm::vec3 newFront = glm::normalize(newOri * glm::vec3(0, 0, -1));
            if (newFront.y > 0.985f || newFront.y < -0.985f) {
                // Only apply the yaw part at the poles.
                newOri = glm::normalize(qYaw * Orientation);
            }
        }
        Orientation = newOri;
        updateVectors();
    }

private:
    void updateVectors() {
        Front = glm::normalize(Orientation * glm::vec3(0.0f, 0.0f, -1.0f));
        Right = glm::normalize(Orientation * glm::vec3(1.0f, 0.0f, 0.0f));
        Up    = glm::normalize(Orientation * glm::vec3(0.0f, 1.0f, 0.0f));
    }
};
