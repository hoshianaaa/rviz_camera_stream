/*
 * Copyright (c) 2009, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "camera_display.h"


#include "rviz/bit_allocator.h"
#include "rviz/display_context.h"
#include "rviz/frame_manager.h"
#include "rviz/load_resource.h"
#include "rviz/ogre_helpers/axes.h"
#include "rviz/properties/display_group_visibility_property.h"
#include "rviz/properties/enum_property.h"
#include "rviz/properties/float_property.h"
#include "rviz/properties/int_property.h"
#include "rviz/properties/property.h"
#include "rviz/properties/ros_topic_property.h"
#include "rviz/render_panel.h"
#include "rviz/uniform_string_stream.h"
#include "rviz/validate_floats.h"

#include <tf/transform_listener.h>

#include <boost/bind.hpp>

#include <rviz/ogre_helpers/axes.h>

#include <OGRE/OgreSceneNode.h>
#include <OGRE/OgreSceneManager.h>
#include <OGRE/OgreRectangle2D.h>
#include <OGRE/OgreMaterialManager.h>
#include <OGRE/OgreTextureManager.h>
#include <OGRE/OgreViewport.h>
#include <OGRE/OgreRenderWindow.h>
#include <OGRE/OgreManualObject.h>
#include <OGRE/OgreRoot.h>
#include <OGRE/OgreRenderSystem.h>

#include <image_transport/image_transport.h>
#include <image_transport/camera_common.h>
#include <sensor_msgs/image_encodings.h>
namespace video_export
{
class VideoPublisher
{
private:
  ros::NodeHandle nh_;
  image_transport::ImageTransport it_;
  image_transport::Publisher pub_;
  uint image_id_;
public:
  VideoPublisher() :
          it_(nh_),
          image_id_(0)
  {
  }
  void shutdown()
  {
    if (pub_.getTopic() != "")
    {
      pub_.shutdown();
    }
  }

  void advertise(std::string topic)
  {
    pub_ = it_.advertise(topic, 1);
  }

  void publishFrame(Ogre::RenderWindow * render_window)
  {
    if (pub_.getTopic() == "")
    {
      return;
    }
    // RenderTarget::writeContentsToFile() used as example
    int height = render_window->getHeight();
    int width = render_window->getWidth();
    Ogre::PixelFormat pf = render_window->suggestPixelFormat();
    uint pixelsize = Ogre::PixelUtil::getNumElemBytes(pf);
    uint datasize = width * height * pixelsize;

    // 1.05 multiplier is to avoid crash when the window is resized.
    // There should be a better solution.
    uchar *data = OGRE_ALLOC_T(uchar, datasize * 1.05, Ogre::MEMCATEGORY_RENDERSYS);
    Ogre::PixelBox pb(width, height, 1, pf, data);
    render_window->copyContentsToMemory(pb);

    sensor_msgs::Image image;
    image.header.stamp = ros::Time::now();
    image.header.seq = image_id_++;
    image.height = height;
    image.width = width;
    image.step = pixelsize * width;
    image.encoding = sensor_msgs::image_encodings::RGB8; // would break if pf changes
    image.is_bigendian = (OGRE_ENDIAN == OGRE_ENDIAN_BIG);
    image.data.resize(datasize);
    memcpy(&image.data[0], data, datasize);
    pub_.publish(image);

    OGRE_FREE(data, Ogre::MEMCATEGORY_RENDERSYS);
  }

};
}// namespace video_export

namespace rviz{
bool validateFloats(const sensor_msgs::CameraInfo& msg)
{
  bool valid = true;
  valid = valid && validateFloats(msg.D);
  valid = valid && validateFloats(msg.K);
  valid = valid && validateFloats(msg.R);
  valid = valid && validateFloats(msg.P);
  return valid;
}

}
namespace rviz_mod
{

static const std::string IMAGE_POS_BACKGROUND = "background";
static const std::string IMAGE_POS_OVERLAY = "overlay";
static const std::string IMAGE_POS_BOTH = "background & overlay";

CameraDisplay::CameraDisplay()
  : Display()
  , zoom_(1)
  , transport_("raw")
  , image_position_(IMAGE_POS_BOTH)
  , caminfo_tf_filter_( 0 )
  , new_caminfo_(false)
  // , texture_(update_nh_)
  , render_panel_( 0 )
  , force_render_(false)
  , video_publisher_( 0 )
{
}

CameraDisplay::~CameraDisplay()
{
  if ( initialized() )
  {
    render_panel_->getRenderWindow()->removeListener( this );

    unsubscribe();
    caminfo_tf_filter_->clear();


    //workaround. delete results in a later crash
    render_panel_->hide();
    //delete render_panel_;

    delete bg_screen_rect_;
    delete fg_screen_rect_;

    bg_scene_node_->getParentSceneNode()->removeAndDestroyChild( bg_scene_node_->getName() );
    fg_scene_node_->getParentSceneNode()->removeAndDestroyChild( fg_scene_node_->getName() );

    delete caminfo_tf_filter_;

    context_->visibilityBits()->freeBits(vis_bit_);
  }
}

void CameraDisplay::onInitialize()
{
  video_publisher_ = new video_export::VideoPublisher();

  caminfo_tf_filter_ = new tf::MessageFilter<sensor_msgs::CameraInfo>(*context_->getTFClient(), "", 2, update_nh_);

  bg_scene_node_ = scene_manager_->getRootSceneNode()->createChildSceneNode();
  fg_scene_node_ = scene_manager_->getRootSceneNode()->createChildSceneNode();

  {
    static int count = 0;
    UniformStringStream ss;
    ss << "RvizModCameraDisplayObject" << count++;

    //background rectangle
    bg_screen_rect_ = new Ogre::Rectangle2D(true);
    bg_screen_rect_->setCorners(-1.0f, 1.0f, 1.0f, -1.0f);

    ss << "Material";
    bg_material_ = Ogre::MaterialManager::getSingleton().create( ss.str(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME );
    bg_material_->setDepthWriteEnabled(false);

    bg_material_->setReceiveShadows(false);
    bg_material_->setDepthCheckEnabled(false);

    bg_material_->getTechnique(0)->setLightingEnabled(false);
    Ogre::TextureUnitState* tu = bg_material_->getTechnique(0)->getPass(0)->createTextureUnitState();
    tu->setTextureName(texture_.getTexture()->getName());
    tu->setTextureFiltering( Ogre::TFO_NONE );
    tu->setAlphaOperation( Ogre::LBX_SOURCE1, Ogre::LBS_MANUAL, Ogre::LBS_CURRENT, 0.0 );

    bg_material_->setCullingMode(Ogre::CULL_NONE);
    bg_material_->setSceneBlending( Ogre::SBT_REPLACE );

    Ogre::AxisAlignedBox aabInf;
    aabInf.setInfinite();

    bg_screen_rect_->setRenderQueueGroup(Ogre::RENDER_QUEUE_BACKGROUND);
    bg_screen_rect_->setBoundingBox(aabInf);
    bg_screen_rect_->setMaterial(bg_material_->getName());

    bg_scene_node_->attachObject(bg_screen_rect_);
    bg_scene_node_->setVisible(false);

    //overlay rectangle
    fg_screen_rect_ = new Ogre::Rectangle2D(true);
    fg_screen_rect_->setCorners(-1.0f, 1.0f, 1.0f, -1.0f);

    fg_material_ = bg_material_->clone( ss.str()+"fg" );
    fg_screen_rect_->setBoundingBox(aabInf);
    fg_screen_rect_->setMaterial(fg_material_->getName());

    fg_material_->setSceneBlending( Ogre::SBT_TRANSPARENT_ALPHA );
    fg_screen_rect_->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);

    fg_scene_node_->attachObject(fg_screen_rect_);
    fg_scene_node_->setVisible(false);
  }

  setAlpha( 0.5f );

  render_panel_ = new RenderPanel();
  render_panel_->getRenderWindow()->addListener( this );
  render_panel_->getRenderWindow()->setAutoUpdated(false);
  render_panel_->getRenderWindow()->setActive( false );
  render_panel_->resize( 640, 480 );
  render_panel_->initialize(context_->getSceneManager(), context_);

  setAssociatedWidget(render_panel_);

  render_panel_->setAutoRender(false);
  render_panel_->setOverlaysEnabled(false);
  render_panel_->getCamera()->setNearClipDistance( 0.01f );

  caminfo_tf_filter_->connectInput(caminfo_sub_);
  caminfo_tf_filter_->registerCallback(boost::bind(&CameraDisplay::caminfoCallback, this, _1));
  // context_->getFrameManager()->registerFilterForTransformStatusCheck(caminfo_tf_filter_, this);

  vis_bit_ = context_->visibilityBits()->allocBit();
  render_panel_->getViewport()->setVisibilityMask( vis_bit_ );

  visibility_property_ = new rviz::DisplayGroupVisibilityProperty(
      vis_bit_, context_->getRootDisplayGroup(), this, "Visibility", true,
      "Changes the visibility of other Displays in the camera view.");

  visibility_property_->setIcon( loadPixmap("package://rviz/icons/visibility.svg",true) );

  this->addChild( visibility_property_, 0 );
}

void CameraDisplay::preRenderTargetUpdate(const Ogre::RenderTargetEvent& evt)
{
  bg_scene_node_->setVisible( image_position_ == IMAGE_POS_BACKGROUND || image_position_ == IMAGE_POS_BOTH );
  fg_scene_node_->setVisible( image_position_ == IMAGE_POS_OVERLAY || image_position_ == IMAGE_POS_BOTH );
}

void CameraDisplay::postRenderTargetUpdate(const Ogre::RenderTargetEvent& evt)
{
  bg_scene_node_->setVisible(false);
  fg_scene_node_->setVisible(false);

  // Hack around and issue that manifests for certain window widths.
  // setSizeIncrement does not work on X
  if ((render_panel_->getRenderWindow()->getWidth() / 2) % 2 == 1)
  {
    render_panel_->setFixedWidth(render_panel_->width() + 2);
  }
  render_panel_->setMaximumWidth(QWIDGETSIZE_MAX);
  render_panel_->setMinimumWidth(0);

  // Publish the rendered window video stream
  video_publisher_->publishFrame(render_panel_->getRenderWindow());
}

void CameraDisplay::onEnable()
{
  subscribe();
  render_panel_->getRenderWindow()->setActive(true);
}

void CameraDisplay::onDisable()
{
  render_panel_->getRenderWindow()->setActive(false);
  unsubscribe();
  clear();
}

void CameraDisplay::subscribe()
{
  if ( !isEnabled() )
  {
    return;
  }

  // std::string target_frame = fixed_frame_.toStdString();
  // ImageDisplayBase::enableTFFilter(target_frame);

  // ImageDisplayBase::subscribe();

  std::string topic = topic_property_->getTopicStd();
  std::string caminfo_topic = image_transport::getCameraInfoTopic(topic_property_->getTopicStd());

  try
  {
    caminfo_sub_.subscribe( update_nh_, caminfo_topic, 1 );
    // setStatus( StatusProperty::Ok, "Camera Info", "OK" );
  }
  catch( ros::Exception& e )
  {
    // setStatus( StatusProperty::Error, "Camera Info", QString( "Error subscribing: ") + e.what() );
  }
}

void CameraDisplay::unsubscribe()
{
  // texture_.setTopic("");
  caminfo_sub_.unsubscribe();
}

void CameraDisplay::setAlpha( float alpha )
{
  alpha_ = alpha;

  Ogre::Pass* pass = fg_material_->getTechnique(0)->getPass(0);
  if (pass->getNumTextureUnitStates() > 0)
  {
    Ogre::TextureUnitState* tex_unit = pass->getTextureUnitState(0);
    tex_unit->setAlphaOperation( Ogre::LBX_MODULATE, Ogre::LBS_MANUAL, Ogre::LBS_CURRENT, alpha_ );
  }
  else
  {
    fg_material_->setAmbient(Ogre::ColourValue(0.0f, 1.0f, 1.0f, alpha_));
    fg_material_->setDiffuse(Ogre::ColourValue(0.0f, 1.0f, 1.0f, alpha_));
  }

  force_render_ = true;
  context_->queueRender();
}

void CameraDisplay::setZoom( float zoom )
{
  if (fabs(zoom) < .00001 || fabs(zoom) > 100000)
  {
    return;
  }
  zoom_ = zoom;


  force_render_ = true;
  context_->queueRender();
}

void CameraDisplay::setQueueSize( int size )
{
  if( size != (int) caminfo_tf_filter_->getQueueSize() )
  {
    // texture_.setQueueSize( (uint32_t) size );
    caminfo_tf_filter_->setQueueSize( (uint32_t) size );
  }
}

int CameraDisplay::getQueueSize()
{
  return (int) caminfo_tf_filter_->getQueueSize();
}

void CameraDisplay::setTopic( const std::string& topic )
{
  unsubscribe();

  topic_ = topic;
  clear();

  subscribe();

}

void CameraDisplay::setOutTopic( const std::string& out_topic )
{
  video_publisher_->shutdown();

  out_topic_ = out_topic;

  if (out_topic_ != "")
  {
    video_publisher_->advertise(out_topic);
  }

}

void CameraDisplay::setTransport(const std::string& transport)
{
  transport_ = transport;

  // texture_.setTransportType(transport);

}

void CameraDisplay::setImagePosition(const std::string& image_position)
{
  image_position_ = image_position;


  force_render_ = true;
  context_->queueRender();
}

void CameraDisplay::clear()
{
  texture_.clear();
  force_render_ = true;
  context_->queueRender();

  new_caminfo_ = false;
  current_caminfo_.reset();

  setStatus(StatusProperty::Warn, "Camera Info", "No CameraInfo received on [" + QString::fromStdString(caminfo_sub_.getTopic()) + "].  Topic may not exist.");
  setStatus(StatusProperty::Warn, "Image", "No Image received");

  render_panel_->getCamera()->setPosition(Ogre::Vector3(999999, 999999, 999999));
}

void CameraDisplay::updateStatus()
{
}

void CameraDisplay::update(float wall_dt, float ros_dt)
{
  updateStatus();

  try
  {
    if (texture_.update() || force_render_)
    {
      float old_alpha = alpha_;

      updateCamera();
      render_panel_->getRenderWindow()->update();
      alpha_ = old_alpha;

      force_render_ = false;
    }
  }
  catch (UnsupportedImageEncoding& e)
  {
    setStatus(StatusProperty::Error, "Image", e.what());
  }
}

void CameraDisplay::updateCamera()
{
  sensor_msgs::CameraInfo::ConstPtr info;
  sensor_msgs::Image::ConstPtr image;
  {
    boost::mutex::scoped_lock lock(caminfo_mutex_);

    info = current_caminfo_;
    image = texture_.getImage();
  }

  if (!info || !image)
  {
    return;
  }

  if (!validateFloats(*info))
  {
    setStatus(StatusProperty::Error, "Camera Info", "Contains invalid floating point values (nans or infs)");
    return;
  }

  Ogre::Vector3 position;
  Ogre::Quaternion orientation;
  //uses the latest TF info to make sure 3D rendered view is up to date with rendered robot pose
  context_->getFrameManager()->getTransform(image->header.frame_id, ros::Time(0), position, orientation);

  // convert vision (Z-forward) frame to ogre frame (Z-out)
  orientation = orientation * Ogre::Quaternion(Ogre::Degree(180), Ogre::Vector3::UNIT_X);

  float img_width = info->width;
  float img_height = info->height;

  // If the image width is 0 due to a malformed caminfo, try to grab the width from the image.
  if (img_width == 0)
  {
    ROS_DEBUG("Malformed CameraInfo on camera [%s], width = 0", getName().toStdString().c_str());

    img_width = texture_.getWidth();
  }

  if (img_height == 0)
  {
    ROS_DEBUG("Malformed CameraInfo on camera [%s], height = 0", getName().toStdString().c_str());

    img_height = texture_.getHeight();
  }

  if (img_height == 0.0 || img_width == 0.0)
  {
    setStatus(StatusProperty::Error, "Camera Info", "Could not determine width/height of image due to malformed CameraInfo (either width or height is 0)");
    return;
  }

  double fx = info->P[0];
  double fy = info->P[5];

  float win_width = render_panel_->width();
  float win_height = render_panel_->height();
  float zoom_x = zoom_;
  float zoom_y = zoom_;

  //preserve aspect ratio
  if ( win_width != 0 && win_height != 0 )
  {
    float img_aspect = (img_width/fx) / (img_height/fy);
    float win_aspect = win_width / win_height;

    if ( img_aspect > win_aspect )
    {
      zoom_y = zoom_y / img_aspect * win_aspect;
    }
    else
    {
      zoom_x = zoom_x / win_aspect * img_aspect;
    }
  }

  // Add the camera's translation relative to the left camera (from P[3]);
  double tx = -1 * (info->P[3] / fx);
  Ogre::Vector3 right = orientation * Ogre::Vector3::UNIT_X;
  position = position + (right * tx);

  double ty = -1 * (info->P[7] / fy);
  Ogre::Vector3 down = orientation * Ogre::Vector3::UNIT_Y;
  position = position + (down * ty);

  if (!validateFloats(position))
  {
    setStatus(StatusProperty::Error, "Camera Info", "CameraInfo/P resulted in an invalid position calculation (nans or infs)");
    return;
  }

  render_panel_->getCamera()->setPosition(position);
  render_panel_->getCamera()->setOrientation(orientation);

  // calculate the projection matrix
  double cx = info->P[2];
  double cy = info->P[6];

  double far_plane = 100;
  double near_plane = 0.01;

  Ogre::Matrix4 proj_matrix;
  proj_matrix = Ogre::Matrix4::ZERO;
 
  proj_matrix[0][0]= 2.0 * fx/img_width * zoom_x;
  proj_matrix[1][1]= 2.0 * fy/img_height * zoom_y;

  proj_matrix[0][2]= 2.0 * (0.5 - cx/img_width) * zoom_x;
  proj_matrix[1][2]= 2.0 * (cy/img_height - 0.5) * zoom_y;

  proj_matrix[2][2]= -(far_plane+near_plane) / (far_plane-near_plane);
  proj_matrix[2][3]= -2.0*far_plane*near_plane / (far_plane-near_plane);

  proj_matrix[3][2]= -1;

  render_panel_->getCamera()->setCustomProjectionMatrix( true, proj_matrix );

  setStatus(StatusProperty::Ok, "Camera Info", "OK");

#if 0
  static Axes* debug_axes = new Axes(scene_manager_, 0, 0.2, 0.01);
  debug_axes->setPosition(position);
  debug_axes->setOrientation(orientation);
#endif

  //adjust the image rectangles to fit the zoom & aspect ratio
  bg_screen_rect_->setCorners(-1.0f*zoom_x, 1.0f*zoom_y, 1.0f*zoom_x, -1.0f*zoom_y);
  fg_screen_rect_->setCorners(-1.0f*zoom_x, 1.0f*zoom_y, 1.0f*zoom_x, -1.0f*zoom_y);

  Ogre::AxisAlignedBox aabInf;
  aabInf.setInfinite();
  bg_screen_rect_->setBoundingBox(aabInf);
  fg_screen_rect_->setBoundingBox(aabInf);
}

void CameraDisplay::caminfoCallback(const sensor_msgs::CameraInfo::ConstPtr& msg)
{
  boost::mutex::scoped_lock lock(caminfo_mutex_);
  current_caminfo_ = msg;
  new_caminfo_ = true;
}

void CameraDisplay::onTransportEnumOptions(QString& choices)
{
  // texture_.getAvailableTransportTypes(choices);
}

void CameraDisplay::onImagePositionEnumOptions(QString& choices)
{
  choices.clear();
  choices.push_back(QString::fromStdString(IMAGE_POS_BACKGROUND));
  choices.push_back(QString::fromStdString(IMAGE_POS_OVERLAY));
  choices.push_back(QString::fromStdString(IMAGE_POS_BOTH));
}

void CameraDisplay::fixedFrameChanged()
{
  caminfo_tf_filter_->setTargetFrame(fixed_frame_.toStdString());
  // texture_.setFrame(fixed_frame_, context_->getTFClient());
}

void CameraDisplay::reset()
{
  Display::reset();

  clear();
}

} // namespace rviz_mod


#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(rviz_mod::CameraDisplay, rviz::Display)