/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <Servo2D.h>
#include <lumin/node/RootNode.h>
#include <lumin/node/QuadNode.h>
#include <lumin/ui/Cursor.h>
#include <ml_logging.h>
#include <scenesGen.h>
#include <SceneDescriptor.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <string.h>

// The viewport dimensions (in px).
const int VIEWPORT_W = 500;
const int VIEWPORT_H = 500;

// The hidpi factor.
const float HIDPI = 1.0;

// The prism dimensions (in m).
const float PRISM_W = 2.0;
const float PRISM_H = 2.0;
const float PRISM_D = 2.0;

// The length of the laser pointer (in m).
const float LASER_LENGTH = 10.0;

// A function which calls the ML logger, suitable for passing into Servo
typedef void (*MLLogger)(MLLogLevel lvl, char* msg);
void logger(MLLogLevel lvl, char* msg) {
  if (MLLoggingLogLevelIsEnabled(lvl)) {
    MLLoggingLog(lvl, ML_DEFAULT_LOG_TAG, msg);
  }
}

// A function which updates the history ui, suitable for passing into Servo
typedef void (*MLHistoryUpdate)(Servo2D* app, bool canGoBack, char* url, bool canGoForward);
void history(Servo2D* app, bool canGoBack, char* url, bool canGoForward) {
  app->updateHistory(canGoBack, url, canGoForward);
}

// The functions Servo provides for hooking up to the ML.
extern "C" ServoInstance* init_servo(EGLContext, EGLSurface, EGLDisplay,
                                     Servo2D*, MLLogger, MLHistoryUpdate,
                                     const char* url, int width, int height, float hidpi);
extern "C" void heartbeat_servo(ServoInstance*);
extern "C" void trigger_servo(ServoInstance*, float x, float y, bool down);
extern "C" void move_servo(ServoInstance*, float x, float y);
extern "C" void traverse_servo(ServoInstance*, int delta);
extern "C" void navigate_servo(ServoInstance*, const char* text);
extern "C" void discard_servo(ServoInstance*);

// Create a Servo2D instance
Servo2D::Servo2D() {
  ML_LOG(Debug, "Servo2D Constructor.");
}

// Destroy a Servo 2D instance
Servo2D::~Servo2D() {
  ML_LOG(Debug, "Servo2D Destructor.");
}

// The prism dimensions
const glm::vec3 Servo2D::getInitialPrismExtents() const {
  return glm::vec3(PRISM_W, PRISM_H, PRISM_D);
}

// Create the prism for Servo
int Servo2D::createInitialPrism() {
  prism_ = requestNewPrism(getInitialPrismExtents());
  if (!prism_) {
    ML_LOG(Error, "Servo2D Error creating default prism.");
    return 1;
  }
  return 0;
}

// Initialize a Servo instance
int Servo2D::init() {

  ML_LOG(Debug, "Servo2D Initializing.");

  // Set up the prism
  createInitialPrism();
  lumin::ui::Cursor::SetScale(prism_, 0.03f);
  instanceInitialScenes();

  // Get the planar resource that holds the EGL context
  lumin::RootNode* root_node = prism_->getRootNode();
  if (!root_node) {
    ML_LOG(Error, "Servo2D Failed to get root node");
    abort();
    return 1;
  }

  std::string content_node_id = Servo2D_exportedNodes::content;
  content_node_ = lumin::QuadNode::CastFrom(prism_->findNode(content_node_id, root_node));
  if (!content_node_) {
    ML_LOG(Error, "Servo2D Failed to get content node");
    abort();
    return 1;
  }
  content_node_->setTriggerable(true);

  content_panel_ = lumin::ui::UiPanel::CastFrom(prism_->findNode(Servo2D_exportedNodes::contentPanel, root_node));
  if (!content_panel_) {
    ML_LOG(Error, "Servo2D Failed to get content panel");
    abort();
    return 1;
  }
  lumin::ui::UiPanel::RequestFocus(content_panel_);

  lumin::ResourceIDType plane_id = prism_->createPlanarEGLResourceId();
  if (!plane_id) {
    ML_LOG(Error, "Servo2D Failed to create EGL resource");
    abort();
    return 1;
  }

  plane_ = static_cast<lumin::PlanarResource*>(prism_->getResource(plane_id));
  if (!plane_) {
    ML_LOG(Error, "Servo2D Failed to create plane");
    abort();
    return 1;
  }

  content_node_->setRenderResource(plane_id);

  // Get the EGL context, surface and display.
  EGLContext ctx = plane_->getEGLContext();
  EGLSurface surf = plane_->getEGLSurface();
  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);

  // Hook into servo
  servo_ = init_servo(ctx, surf, dpy, this, logger, history, "https://servo.org/", VIEWPORT_H, VIEWPORT_W, HIDPI);
  if (!servo_) {
    ML_LOG(Error, "Servo2D Failed to init servo instance");
    abort();
    return 1;
  }

  // Add a callback to the back button
  std::string back_button_id = Servo2D_exportedNodes::backButton;
  back_button_ = lumin::ui::UiButton::CastFrom(prism_->findNode(back_button_id, root_node));
  if (!back_button_) {
    ML_LOG(Error, "Servo2D Failed to get back button");
    abort();
    return 1;
  }
  back_button_->onActivateSub(std::bind(traverse_servo, servo_, -1));

  // Add a callback to the forward button
  std::string fwd_button_id = Servo2D_exportedNodes::fwdButton;
  fwd_button_ = lumin::ui::UiButton::CastFrom(prism_->findNode(fwd_button_id, root_node));
  if (!fwd_button_) {
    ML_LOG(Error, "Servo2D Failed to get forward button");
    abort();
    return 1;
  }
  fwd_button_->onActivateSub(std::bind(traverse_servo, servo_, +1));

  // Add a callback to the URL bar
  std::string url_bar_id = Servo2D_exportedNodes::urlBar;
  url_bar_ = lumin::ui::UiTextEdit::CastFrom(prism_->findNode(url_bar_id, root_node));
  if (!url_bar_) {
    ML_LOG(Error, "Servo2D Failed to get URL bar");
    abort();
    return 1;
  }
  url_bar_->onFocusLostSub(std::bind(&Servo2D::urlBarEventListener, this));

  // Add the laser pointer
  laser_ = lumin::LineNode::CastFrom(prism_->findNode(Servo2D_exportedNodes::laser, root_node));
  if (!laser_) {
    ML_LOG(Error, "Servo2D Failed to get laser");
    abort();
    return 1;
  }

  return 0;
}

int Servo2D::deInit() {
  ML_LOG(Debug, "Servo2D Deinitializing.");
  discard_servo(servo_);
  servo_ = nullptr;
  return 0;
}

lumin::Node* Servo2D::instanceScene(const SceneDescriptor& scene) {
  // Load resources.
  if (!prism_->loadResourceModel(scene.getResourceModelPath())) {
    ML_LOG(Info, "No resource model loaded");
  }

  // Load a scene file.
  std::string editorObjectModelName;
  if (!prism_->loadObjectModel(scene.getSceneGraphPath(), editorObjectModelName)) {
    ML_LOG(Error, "Servo2D Failed to load object model");
    abort();
    return nullptr;
  }

  // Add scene to this prism.
  lumin::Node* newTree = prism_->createAll(editorObjectModelName);
  if (!prism_->getRootNode()->addChild(newTree)) {
    ML_LOG(Error, "Servo2D Failed to add newTree to the prism root node");
    abort();
    return nullptr;
  }

  return newTree;
}

void Servo2D::instanceInitialScenes() {
  // Iterate over all the exported scenes
  for (auto& exportedSceneEntry : scenes::exportedScenes ) {

    // If this scene was marked to be instanced at app initialization, do it
    const SceneDescriptor &sd = exportedSceneEntry.second;
    if (sd.getInitiallyInstanced()) {
      instanceScene(sd);
    }
  }
}

bool Servo2D::updateLoop(float fDelta) {
  glm::vec2 pos = redrawLaser();
  move_servo(servo_, pos.x, pos.y);
  heartbeat_servo(servo_);
  return true;
}

bool Servo2D::eventListener(lumin::ServerEvent* event) {
  // Dispatch based on event type
  lumin::ServerEventType typ = event->getServerEventType();
  switch (typ) {
    case lumin::ServerEventType::kGestureInputEvent:
      return gestureEventListener(static_cast<lumin::GestureInputEventData*>(event));
    case lumin::ServerEventType::kControlPose6DofInputEvent:
      return pose6DofEventListener(static_cast<lumin::ControlPose6DofInputEventData*>(event));
    default:
      return false;
  }
}

glm::vec2 Servo2D::viewportPosition(glm::vec3 prism_pos) {
  // Get the cursor position relative to the origin of the content node (in m)
  glm::vec3 pos = prism_pos - content_node_->getPrismPosition();

  // Get the size of the content node (in m)
  glm::vec2 sz = content_node_->getSize();

  // Convert to a position in viewport px
  float x = (pos.x / sz.x) * (float)VIEWPORT_W;
  float y = (1 - pos.y / sz.y) * (float)VIEWPORT_H; // Sigh, invert the y coordinate

  return glm::vec2(x, y);
}

bool Servo2D::pointInsideViewport(glm::vec2 pt) {
   return (0 <= pt.x && 0 <= pt.y && pt.x <= VIEWPORT_W && pt.y <= VIEWPORT_H);
}

bool Servo2D::pose6DofEventListener(lumin::ControlPose6DofInputEventData* event) {
  // Get the controller position in world coordinates
  event->get6DofPosition(controller_position_.x, controller_position_.y, controller_position_.z);
  // Get the controller orientation
  event->getQuaternion(controller_orientation_.w, controller_orientation_.x,
                       controller_orientation_.y, controller_orientation_.z);
  // Bubble up to any other 6DOF handlers
  return false;
}

glm::vec2 Servo2D::redrawLaser() {
  // Return (-1, -1) if the laser doesn't intersect z=0
  glm::vec2 result = glm::vec2(-1.0, -1.0);

  // Convert to prism coordinates
  glm::vec3 position = glm::inverse(prism_->getTransform()) * glm::vec4(controller_position_, 1.0f);
  glm::quat orientation = glm::inverse(prism_->getRotation()) * controller_orientation_;

  // 1m in the direction of the controller
  glm::vec3 direction = orientation * glm::vec3(0.0f, 0.0f, -1.0f);
  // The endpoint of the laser, in prism coordinates
  glm::vec3 endpoint = position + direction * LASER_LENGTH;

  // The laser color
  glm::vec4 color = glm::vec4(0.0, 0.0, 0.0, 0.0);

  // Check to see if the cursor is over the content
  glm::vec2 cursor = viewportPosition(lumin::ui::Cursor::GetPosition(prism_));

  // Is the laser active and does the laser intersect z=0?
  if (pointInsideViewport(cursor) && ((position.z < 0) ^ (endpoint.z < 0))) {
    // How far along the laser did it intersect?
    float ratio = 1.0 / (1.0 - (endpoint.z / position.z));
    // The intersection point
    glm::vec3 intersection = ((1 - ratio) * position) + (ratio * endpoint);
    // Is the intersection inside the viewport?
    result = viewportPosition(intersection);
    if (pointInsideViewport(result)) {
      color = glm::vec4(0.0, 1.0, 0.0, 1.0);
      endpoint = intersection;
    } else {
      color = glm::vec4(1.0, 0.0, 0.0, 1.0);
    }
  }

  laser_->clearPoints();
  laser_->addPoints(position);
  laser_->addPoints(endpoint);
  laser_->setColor(color);
  return result;
}

bool Servo2D::gestureEventListener(lumin::GestureInputEventData* event) {
  // Only respond to trigger up or down
  lumin::input::GestureType typ = event->getGesture();
  if (typ != lumin::input::GestureType::TriggerDown && typ != lumin::input::GestureType::TriggerUp) {
    return false;
  }

  // Only respond when the cursor is enabled
  if (!lumin::ui::Cursor::IsEnabled(prism_)) {
    return false;
  }

  // Only respond when the cursor is inside the viewport
  glm::vec2 cursor = viewportPosition(lumin::ui::Cursor::GetPosition(prism_));
  if (!pointInsideViewport(cursor)) {
    return false;
  }

  // Inform Servo of the trigger
  glm::vec2 pos = redrawLaser();
  trigger_servo(servo_, pos.x, pos.y, typ == lumin::input::GestureType::TriggerDown);
  return true;
}

void Servo2D::urlBarEventListener() {
  navigate_servo(servo_, url_bar_->getText().c_str());
}

void Servo2D::updateHistory(bool canGoBack, const char* url, bool canGoForward) {
  back_button_->setEnabled(canGoBack);
  fwd_button_->setEnabled(canGoForward);
  url_bar_->setText(url);
}
