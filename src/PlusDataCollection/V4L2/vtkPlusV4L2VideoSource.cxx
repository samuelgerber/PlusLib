/*=Plus=header=begin======================================================
 Program: Plus
 Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
 See License.txt for details.
 =========================================================Plus=header=end*/

// Local includes
#include "PlusConfigure.h"
#include "vtkPlusV4L2VideoSource.h"
#include "vtkPlusChannel.h"
#include "vtkPlusDataSource.h"

// VTK includes
#include <vtkImageData.h>
#include <vtkObjectFactory.h>

// V4L2 includes
#include <linux/videodev2.h>

// OS includes
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

//----------------------------------------------------------------------------

vtkStandardNewMacro(vtkPlusV4L2VideoSource);

//----------------------------------------------------------------------------

namespace
{
  int xioctl(int fh, unsigned long int request, void* arg)
  {
    int r;

    do
    {
      r = ioctl(fh, request, arg);
    }
    while (-1 == r && EINTR == errno);

    return r;
  }
}

#define CLEAR(x) memset(&(x), 0, sizeof(x))

//----------------------------------------------------------------------------
vtkPlusV4L2VideoSource::vtkPlusV4L2VideoSource()
  : DeviceName("")
  , IOMethod(IO_METHOD_READ)
  , FileDescriptor(-1)
  , Frames(nullptr)
  , BufferCount(0)
  , RequestedFormat(std::make_shared<v4l2_format>())
{

}

//----------------------------------------------------------------------------
vtkPlusV4L2VideoSource::~vtkPlusV4L2VideoSource()
{
}

//----------------------------------------------------------------------------
void vtkPlusV4L2VideoSource::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "DeviceName: " << this->DeviceName << std::endl;
  os << indent << "IOMethod: " << this->IOMethodToString(this->IOMethod) << std::endl;
  os << indent << "BufferCount: " << this->BufferCount << std::endl;

  if (this->FileDescriptor != -1)
  {
    os << indent << "Available formats: " << std::endl;

    v4l2_fmtdesc fmtdesc;
    CLEAR(fmtdesc);
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (ioctl(this->FileDescriptor, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
    {
      os << indent << fmtdesc.description << std::endl;
      fmtdesc.index++;
    }
  }
  else
  {
    os << indent << "Cannot enumerate known formats. Camera not connected." << std::endl;
  }
}

//-----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::ReadConfiguration(vtkXMLDataElement* rootConfigElement)
{
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_READING(deviceConfig, rootConfigElement);

  XML_READ_STRING_ATTRIBUTE_REQUIRED(DeviceName, deviceConfig);
  std::string ioMethod;
  XML_READ_STRING_ATTRIBUTE_NONMEMBER_OPTIONAL(IOMethod, ioMethod, rootConfigElement);
  if (vtkPlusV4L2VideoSource::StringToIOMethod(ioMethod) != IO_METHOD_UNKNOWN)
  {
    this->IOMethod = vtkPlusV4L2VideoSource::StringToIOMethod(ioMethod);
  }
  else
  {
    LOG_WARNING("Unknown method: " << ioMethod << ". Defaulting to " << vtkPlusV4L2VideoSource::IOMethodToString(this->IOMethod));
  }

  int frameSize[3];
  XML_READ_VECTOR_ATTRIBUTE_NONMEMBER_OPTIONAL(int, 3, FrameSize, frameSize, deviceConfig);
  if (deviceConfig->GetAttribute("FrameSize") != nullptr)
  {
    this->RequestedFormat->fmt.pix.width = frameSize[0];
    this->RequestedFormat->fmt.pix.height = frameSize[1];
  }

  std::string pixelFormat;
  XML_READ_STRING_ATTRIBUTE_NONMEMBER_OPTIONAL("PixelFormat", pixelFormat, deviceConfig);
  if (deviceConfig->GetAttribute("PixelFormat") != nullptr)
  {
    this->RequestedFormat->fmt.pix.pixelformat = vtkPlusV4L2VideoSource::StringToFormat(pixelFormat);
  }

  std::string fieldOrder;
  XML_READ_STRING_ATTRIBUTE_NONMEMBER_OPTIONAL("FieldOrder", fieldOrder, deviceConfig);
  if (deviceConfig->GetAttribute("FieldOrder") != nullptr)
  {
    this->RequestedFormat->fmt.pix.field = vtkPlusV4L2VideoSource::StringToFieldOrder(fieldOrder);
  }

  return PLUS_SUCCESS;
}

//-----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::WriteConfiguration(vtkXMLDataElement* rootConfigElement)
{
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_WRITING(deviceConfig, rootConfigElement);

  XML_WRITE_STRING_ATTRIBUTE_IF_NOT_EMPTY(DeviceName, deviceConfig);
  deviceConfig->SetAttribute("IOMethod", vtkPlusV4L2VideoSource::IOMethodToString(this->IOMethod).c_str());

  return PLUS_SUCCESS;
}

//-----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InitRead(unsigned int bufferSize)
{
  this->Frames = (FrameBuffer*) calloc(1, sizeof(FrameBuffer));

  if (!this->Frames)
  {
    LOG_ERROR("Unable to allocate " << sizeof(FrameBuffer) << " bytes for capture frame.");
    return PLUS_FAIL;
  }

  this->Frames[0].length = bufferSize;
  this->Frames[0].start = malloc(bufferSize);

  if (!this->Frames[0].start)
  {
    LOG_ERROR("Unable to allocate " << bufferSize << " bytes for capture frame.");
    return PLUS_FAIL;
  }

  return PLUS_SUCCESS;
}

//-----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InitMmap()
{
  v4l2_requestbuffers req;

  CLEAR(req);

  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (-1 == xioctl(this->FileDescriptor, VIDIOC_REQBUFS, &req))
  {
    if (EINVAL == errno)
    {
      LOG_ERROR(this->DeviceName << " does not support memory mapping");
    }
    else
    {
      LOG_ERROR("VIDIOC_REQBUFS" << ": " << errno << ", " << strerror(errno));
    }
    return PLUS_FAIL;
  }

  if (req.count < 2)
  {
    LOG_ERROR("Insufficient buffer memory on " << this->DeviceName);
    return PLUS_FAIL;
  }

  this->Frames = (FrameBuffer*) calloc(req.count, sizeof(FrameBuffer));

  if (!this->Frames)
  {
    LOG_ERROR("Out of memory");
    return PLUS_FAIL;
  }

  for (this->BufferCount = 0; this->BufferCount < req.count; ++this->BufferCount)
  {
    v4l2_buffer buf;
    CLEAR(buf);

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = this->BufferCount;

    if (-1 == xioctl(this->FileDescriptor, VIDIOC_QUERYBUF, &buf))
    {
      LOG_ERROR("VIDIOC_QUERYBUF" << ": " << errno << ", " << strerror(errno));
      return PLUS_FAIL;
    }

    this->Frames[this->BufferCount].length = buf.length;
    this->Frames[this->BufferCount].start = mmap(NULL /* start anywhere */, buf.length, PROT_READ | PROT_WRITE /* required */, MAP_SHARED /* recommended */, this->FileDescriptor, buf.m.offset);

    if (MAP_FAILED == this->Frames[this->BufferCount].start)
    {
      LOG_ERROR("mmap" << ": " << errno << ", " << strerror(errno));
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InitUserp(unsigned int bufferSize)
{
  v4l2_requestbuffers req;

  CLEAR(req);

  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_USERPTR;

  if (-1 == xioctl(this->FileDescriptor, VIDIOC_REQBUFS, &req))
  {
    if (EINVAL == errno)
    {
      LOG_ERROR(this->DeviceName << " does not support user pointer i/o");
    }
    else
    {
      LOG_ERROR("VIDIOC_REQBUFS" << ": " << errno << ", " << strerror(errno));
    }
    return PLUS_FAIL;
  }

  this->Frames = (FrameBuffer*) calloc(4, sizeof(FrameBuffer));

  if (!this->Frames)
  {
    LOG_ERROR("Out of memory");
    return PLUS_FAIL;
  }

  for (this->BufferCount = 0; this->BufferCount < 4; ++this->BufferCount)
  {
    this->Frames[this->BufferCount].length = bufferSize;
    this->Frames[this->BufferCount].start = malloc(bufferSize);

    if (!this->Frames[this->BufferCount].start)
    {
      LOG_ERROR("Out of memory");
      return PLUS_FAIL;
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InternalConnect()
{
  // Open the device
  struct stat st;

  if (-1 == stat(this->DeviceName.c_str(), &st))
  {
    LOG_ERROR("Cannot identify " << this->DeviceName << ": " << errno << ", " << strerror(errno));
    return PLUS_FAIL;
  }

  if (!S_ISCHR(st.st_mode))
  {
    LOG_ERROR(this->DeviceName << " is not a valid device.");
    return PLUS_FAIL;
  }

  this->FileDescriptor = open(this->DeviceName.c_str(), O_RDWR | O_NONBLOCK, 0);

  if (-1 == this->FileDescriptor)
  {
    LOG_ERROR("Cannot open " << this->DeviceName << ": " << errno << ", " << strerror(errno));
    return PLUS_FAIL;
  }

  // Confirm requested device is capable
  v4l2_capability cap;
  if (-1 == xioctl(this->FileDescriptor, VIDIOC_QUERYCAP, &cap))
  {
    if (EINVAL == errno)
    {
      LOG_ERROR(this->DeviceName << " is not a V4L2 device");
    }
    else
    {
      LOG_ERROR("VIDIOC_QUERYCAP" << ": " << errno << ", " << strerror(errno));
    }
    return PLUS_FAIL;
  }

  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
  {
    LOG_ERROR(this->DeviceName << " is not a video capture device");
    return PLUS_FAIL;
  }

#if defined(_DEBUG)
  this->PrintSelf(std::cout, vtkIndent());
#endif

  switch (this->IOMethod)
  {
    case IO_METHOD_READ:
      if (!(cap.capabilities & V4L2_CAP_READWRITE))
      {
        LOG_ERROR(this->DeviceName << " does not support read i/o");
        return PLUS_FAIL;
      }
      break;

    case IO_METHOD_MMAP:
    case IO_METHOD_USERPTR:
      if (!(cap.capabilities & V4L2_CAP_STREAMING))
      {
        LOG_ERROR(this->DeviceName << " does not support streaming i/o");
        return PLUS_FAIL;
      }
      break;
  }

  // Select video input, video standard and tune here
  v4l2_cropcap cropcap;
  CLEAR(cropcap);
  cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if (0 == xioctl(this->FileDescriptor, VIDIOC_CROPCAP, &cropcap))
  {
    v4l2_crop crop;
    CLEAR(crop);
    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    crop.c = cropcap.defrect;

    // TODO : get clip information from data source and set to device
    if (-1 == xioctl(this->FileDescriptor, VIDIOC_S_CROP, &crop))
    {
      switch (errno)
      {
        case EINVAL:
          /* Cropping not supported. */
          break;
      }
    }
  }

  // Retrieve current v4l2 format settings
  if (-1 == xioctl(this->FileDescriptor, VIDIOC_G_FMT, this->RequestedFormat.get()))
  {
    LOG_ERROR("VIDIOC_G_FMT" << ": " << errno << ", " << strerror(errno));
    return PLUS_FAIL;
  }

  // TODO: set from configuration
  this->RequestedFormat->fmt.pix.width = 640;
  this->RequestedFormat->fmt.pix.height = 480;
  this->RequestedFormat->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  this->RequestedFormat->fmt.pix.field = V4L2_FIELD_INTERLACED;

  if (-1 == xioctl(this->FileDescriptor, VIDIOC_S_FMT, this->RequestedFormat.get()))
  {
    LOG_ERROR("VIDIOC_S_FMT" << ": " << errno << ", " << strerror(errno));
    return PLUS_FAIL;
  }

  switch (this->IOMethod)
  {
    case IO_METHOD_READ:
    {
      return this->InitRead(this->RequestedFormat->fmt.pix.sizeimage);
    }
    case IO_METHOD_MMAP:
    {
      return this->InitMmap();
    }
    case IO_METHOD_USERPTR:
    {
      return this->InitUserp(this->RequestedFormat->fmt.pix.sizeimage);
    }
    default:
      return PLUS_FAIL;
  }
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InternalDisconnect()
{
  switch (this->IOMethod)
  {
    case IO_METHOD_READ:
    {
      free(this->Frames[0].start);
      break;
    }
    case IO_METHOD_MMAP:
    {
      for (unsigned int i = 0; i < this->BufferCount; ++i)
      {
        if (-1 == munmap(this->Frames[i].start, this->Frames[i].length))
        {
          LOG_ERROR("munmap" << ": " << errno << ", " << strerror(errno));
          return PLUS_FAIL;
        }
      }
      break;
    }
    case IO_METHOD_USERPTR:
    {
      for (unsigned int i = 0; i < this->BufferCount; ++i)
      {
        free(this->Frames[i].start);
      }
      break;
    }
  }

  free(this->Frames);

  if (-1 == close(this->FileDescriptor))
  {
    LOG_ERROR("Close" << ": " << errno << ", " << strerror(errno));
    return PLUS_FAIL;
  }

  this->FileDescriptor = -1;

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InternalUpdate()
{
  fd_set fds;
  timeval tv;
  int r;

  FD_ZERO(&fds);
  FD_SET(this->FileDescriptor, &fds);

  // Timeout
  tv.tv_sec = 2;
  tv.tv_usec = 0;

  r = select(this->FileDescriptor + 1, &fds, NULL, NULL, &tv);

  if (-1 == r)
  {
    LOG_ERROR("Unable to select video device" << ": " << errno << ", " << strerror(errno));
    return PLUS_FAIL;
  }

  if (0 == r)
  {
    LOG_ERROR("Select timeout.");
    return PLUS_FAIL;
  }

  if (this->ReadFrame() != PLUS_SUCCESS)
  {
    return PLUS_FAIL;
  }

  this->FrameNumber++;

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::ReadFrame()
{
  v4l2_buffer buf;

  switch (this->IOMethod)
  {
    case IO_METHOD_READ:
    {
      if (-1 == read(this->FileDescriptor, this->Frames[0].start, this->Frames[0].length))
      {
        switch (errno)
        {
          case EAGAIN:
          {
            return PLUS_FAIL;
          }
          case EIO:
          {
            // Could ignore EIO, see spec
          }
          default:
          {
            LOG_ERROR("Read" << ": " << errno << ", " << strerror(errno));
            return PLUS_FAIL;
          }
        }
      }
      break;
    }
    case IO_METHOD_MMAP:
    {
      CLEAR(buf);

      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;

      if (-1 == xioctl(this->FileDescriptor, VIDIOC_DQBUF, &buf))
      {
        switch (errno)
        {
          case EAGAIN:
          {
            return PLUS_FAIL;
          }
          case EIO:
          {
            // Could ignore EIO, see spec
          }
          default:
          {
            LOG_ERROR("VIDIOC_DQBUF" << ": " << errno << ", " << strerror(errno));
            return PLUS_FAIL;
          }
        }
      }

      if (-1 == xioctl(this->FileDescriptor, VIDIOC_QBUF, &buf))
      {
        LOG_ERROR("VIDIOC_QBUF" << ": " << errno << ", " << strerror(errno));
        return PLUS_FAIL;
      }
      break;
    }
    case IO_METHOD_USERPTR:
    {
      CLEAR(buf);

      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_USERPTR;

      if (-1 == xioctl(this->FileDescriptor, VIDIOC_DQBUF, &buf))
      {
        switch (errno)
        {
          case EAGAIN:
          {
            return PLUS_FAIL;
          }
          case EIO:
          {
            // Could ignore EIO, see spec
          }
          default:
          {
            LOG_ERROR("VIDIOC_DQBUF" << ": " << errno << ", " << strerror(errno));
            return PLUS_FAIL;
          }
        }
      }

      for (unsigned int i = 0; i < this->BufferCount; ++i)
      {
        if (buf.m.userptr == (unsigned long) this->Frames[i].start && buf.length == this->Frames[i].length)
        {
          break;
        }
      }

      if (-1 == xioctl(this->FileDescriptor, VIDIOC_QBUF, &buf))
      {
        LOG_ERROR("VIDIOC_QBUF" << ": " << errno << ", " << strerror(errno));
        return PLUS_FAIL;
      }
      break;
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::NotifyConfigured()
{
  if (this->OutputChannels.empty())
  {
    LOG_ERROR("No output channels defined for vtkPlusV4L2VideoSource. Cannot proceed.");
    this->CorrectlyConfigured = false;
    return PLUS_FAIL;
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InternalStopRecording()
{
  v4l2_buf_type type;

  switch (this->IOMethod)
  {
    case IO_METHOD_READ:
    {
      // Nothing to do
      break;
    }
    case IO_METHOD_MMAP:
    case IO_METHOD_USERPTR:
    {
      type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (-1 == xioctl(this->FileDescriptor, VIDIOC_STREAMOFF, &type))
      {
        LOG_ERROR("VIDIOC_STREAMOFF" << ": " << errno << ", " << strerror(errno));
        break;
      }
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InternalStartRecording()
{
  v4l2_buf_type type;

  switch (this->IOMethod)
  {
    case IO_METHOD_READ:
    {
      // Nothing to do
      break;
    }
    case IO_METHOD_MMAP:
    {
      for (unsigned int i = 0; i < this->BufferCount; ++i)
      {
        v4l2_buffer buf;
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (-1 == xioctl(this->FileDescriptor, VIDIOC_QBUF, &buf))
        {
          LOG_ERROR("VIDIOC_QBUF" << ": " << errno << ", " << strerror(errno));
          return PLUS_FAIL;
        }
      }
      type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (-1 == xioctl(this->FileDescriptor, VIDIOC_STREAMON, &type))
      {
        LOG_ERROR("VIDIOC_STREAMON" << ": " << errno << ", " << strerror(errno));
        return PLUS_FAIL;
      }
      break;
    }
    case IO_METHOD_USERPTR:
    {
      for (unsigned int i = 0; i < this->BufferCount; ++i)
      {
        v4l2_buffer buf;
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = i;
        buf.m.userptr = (unsigned long) this->Frames[i].start;
        buf.length = this->Frames[i].length;

        if (-1 == xioctl(this->FileDescriptor, VIDIOC_QBUF, &buf))
        {
          LOG_ERROR("VIDIOC_QBUF" << ": " << errno << ", " << strerror(errno));
          return PLUS_FAIL;
        }
      }
      type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (-1 == xioctl(this->FileDescriptor, VIDIOC_STREAMON, &type))
      {
        LOG_ERROR("VIDIOC_STREAMON" << ": " << errno << ", " << strerror(errno));
        return PLUS_FAIL;
      }
      break;
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
std::string vtkPlusV4L2VideoSource::IOMethodToString(V4L2_IO_METHOD ioMethod)
{
  switch (ioMethod)
  {
    case IO_METHOD_READ:
      return "IO_METHOD_READ";
    case IO_METHOD_MMAP:
      return "IO_METHOD_MMAP";
    case IO_METHOD_USERPTR:
      return "IO_METHOD_USERPTR";
    default:
      return "IO_METHOD_UNKNOWN";
  }
}

//----------------------------------------------------------------------------
vtkPlusV4L2VideoSource::V4L2_IO_METHOD vtkPlusV4L2VideoSource::StringToIOMethod(const std::string& method)
{
  if (PlusCommon::IsEqualInsensitive(method, "IO_METHOD_READ"))
  {
    return IO_METHOD_READ;
  }
  else if (PlusCommon::IsEqualInsensitive(method, "IO_METHOD_MMAP"))
  {
    return IO_METHOD_MMAP;
  }
  else if (PlusCommon::IsEqualInsensitive(method, "IO_METHOD_USERPTR"))
  {
    return IO_METHOD_USERPTR;
  }
  else
  {
    return IO_METHOD_UNKNOWN;
  }
}

//----------------------------------------------------------------------------
std::string vtkPlusV4L2VideoSource::FormatToString(v4l2_pix_format format)
{
  switch (format)
  {
    case V4L2_PIX_FMT_RGB332:
    {
      return "V4L2_PIX_FMT_RGB332";
    }
    case V4L2_PIX_FMT_RGB444:
    {
      return "V4L2_PIX_FMT_RGB444";
    }
    case V4L2_PIX_FMT_ARGB444:
    {
      return "V4L2_PIX_FMT_ARGB444";
    }
    case V4L2_PIX_FMT_XRGB444:
    {
      return "V4L2_PIX_FMT_XRGB444";
    }
    case V4L2_PIX_FMT_RGB555:
    {
      return "V4L2_PIX_FMT_RGB555";
    }
    case V4L2_PIX_FMT_ARGB555:
    {
      return "V4L2_PIX_FMT_ARGB555";
    }
    case V4L2_PIX_FMT_XRGB555:
    {
      return "V4L2_PIX_FMT_XRGB555";
    }
    case V4L2_PIX_FMT_RGB565:
    {
      return "V4L2_PIX_FMT_RGB565";
    }
    case V4L2_PIX_FMT_RGB555X:
    {
      return "V4L2_PIX_FMT_RGB555X";
    }
    case V4L2_PIX_FMT_ARGB555X:
    {
      return "V4L2_PIX_FMT_ARGB555X";
    }
    case V4L2_PIX_FMT_XRGB555X:
    {
      return "V4L2_PIX_FMT_XRGB555X";
    }
    case V4L2_PIX_FMT_RGB565X:
    {
      return "V4L2_PIX_FMT_RGB565X";
    }
    case V4L2_PIX_FMT_BGR666:
    {
      return "V4L2_PIX_FMT_BGR666";
    }
    case V4L2_PIX_FMT_BGR24:
    {
      return "V4L2_PIX_FMT_BGR24";
    }
    case V4L2_PIX_FMT_RGB24:
    {
      return "V4L2_PIX_FMT_RGB24";
    }
    case V4L2_PIX_FMT_BGR32:
    {
      return "V4L2_PIX_FMT_BGR32";
    }
    case V4L2_PIX_FMT_ABGR32:
    {
      return "V4L2_PIX_FMT_ABGR32";
    }
    case V4L2_PIX_FMT_XBGR32:
    {
      return "V4L2_PIX_FMT_XBGR32";
    }
    case V4L2_PIX_FMT_RGB32:
    {
      return "V4L2_PIX_FMT_RGB32";
    }
    case V4L2_PIX_FMT_ARGB32:
    {
      return "V4L2_PIX_FMT_ARGB32";
    }
    case V4L2_PIX_FMT_XRGB32:
    {
      return "V4L2_PIX_FMT_XRGB32";
    }
    case V4L2_PIX_FMT_GREY:
    {
      return "V4L2_PIX_FMT_GREY";
    }
    case V4L2_PIX_FMT_Y4:
    {
      return "V4L2_PIX_FMT_Y4";
    }
    case V4L2_PIX_FMT_Y6:
    {
      return "V4L2_PIX_FMT_Y6";
    }
    case V4L2_PIX_FMT_Y10:
    {
      return "V4L2_PIX_FMT_Y10";
    }
    case V4L2_PIX_FMT_Y12:
    {
      return "V4L2_PIX_FMT_Y12";
    }
    case V4L2_PIX_FMT_Y16:
    {
      return "V4L2_PIX_FMT_Y16";
    }
    case V4L2_PIX_FMT_Y16_BE:
    {
      return "V4L2_PIX_FMT_Y16_BE";
    }
    case V4L2_PIX_FMT_Y10BPACK:
    {
      return "V4L2_PIX_FMT_Y10BPACK";
    }
    case V4L2_PIX_FMT_PAL8:
    {
      return "V4L2_PIX_FMT_PAL8";
    }
    case V4L2_PIX_FMT_UV8:
    {
      return "V4L2_PIX_FMT_UV8";
    }
    case V4L2_PIX_FMT_YUYV:
    {
      return "V4L2_PIX_FMT_YUYV";
    }
    case V4L2_PIX_FMT_YYUV:
    {
      return "V4L2_PIX_FMT_YYUV";
    }
    case V4L2_PIX_FMT_YVYU:
    {
      return "V4L2_PIX_FMT_YVYU";
    }
    case V4L2_PIX_FMT_UYVY:
    {
      return "V4L2_PIX_FMT_UYVY";
    }
    case V4L2_PIX_FMT_VYUY:
    {
      return "V4L2_PIX_FMT_VYUY";
    }
    case V4L2_PIX_FMT_Y41P:
    {
      return "V4L2_PIX_FMT_Y41P";
    }
    case V4L2_PIX_FMT_YUV444:
    {
      return "V4L2_PIX_FMT_YUV444";
    }
    case V4L2_PIX_FMT_YUV555:
    {
      return "V4L2_PIX_FMT_YUV555";
    }
    case V4L2_PIX_FMT_YUV565:
    {
      return "V4L2_PIX_FMT_YUV565";
    }
    case V4L2_PIX_FMT_YUV32:
    {
      return "V4L2_PIX_FMT_YUV32";
    }
    case V4L2_PIX_FMT_HI240:
    {
      return "V4L2_PIX_FMT_HI240";
    }
    case V4L2_PIX_FMT_HM12:
    {
      return "V4L2_PIX_FMT_HM12";
    }
    case V4L2_PIX_FMT_M420:
    {
      return "V4L2_PIX_FMT_M420";
    }
    case V4L2_PIX_FMT_NV12:
    {
      return "V4L2_PIX_FMT_NV12";
    }
    case V4L2_PIX_FMT_NV21:
    {
      return "V4L2_PIX_FMT_NV21";
    }
    case V4L2_PIX_FMT_NV16:
    {
      return "V4L2_PIX_FMT_NV16";
    }
    case V4L2_PIX_FMT_NV61:
    {
      return "V4L2_PIX_FMT_NV61";
    }
    case V4L2_PIX_FMT_NV24:
    {
      return "V4L2_PIX_FMT_NV24";
    }
    case V4L2_PIX_FMT_NV42:
    {
      return "V4L2_PIX_FMT_NV42";
    }
    case V4L2_PIX_FMT_NV12M:
    {
      return "V4L2_PIX_FMT_NV12M";
    }
    case V4L2_PIX_FMT_NV21M:
    {
      return "V4L2_PIX_FMT_NV21M";
    }
    case V4L2_PIX_FMT_NV16M:
    {
      return "V4L2_PIX_FMT_NV16M";
    }
    case V4L2_PIX_FMT_NV61M:
    {
      return "V4L2_PIX_FMT_NV61M";
    }
    case V4L2_PIX_FMT_NV12MT:
    {
      return "V4L2_PIX_FMT_NV12MT";
    }
    case V4L2_PIX_FMT_NV12MT_16X16:
    {
      return "V4L2_PIX_FMT_NV12MT_16X16";
    }
    case V4L2_PIX_FMT_YUV410:
    {
      return "V4L2_PIX_FMT_YUV410";
    }
    case V4L2_PIX_FMT_YVU410:
    {
      return "V4L2_PIX_FMT_YVU410";
    }
    case V4L2_PIX_FMT_YUV411P:
    {
      return "V4L2_PIX_FMT_YUV411P";
    }
    case V4L2_PIX_FMT_YUV420:
    {
      return "V4L2_PIX_FMT_YUV420";
    }
    case V4L2_PIX_FMT_YVU420:
    {
      return "V4L2_PIX_FMT_YVU420";
    }
    case V4L2_PIX_FMT_YUV422P:
    {
      return "V4L2_PIX_FMT_YUV422P";
    }
    case V4L2_PIX_FMT_YUV420M:
    {
      return "V4L2_PIX_FMT_YUV420M";
    }
    case V4L2_PIX_FMT_YVU420M:
    {
      return "V4L2_PIX_FMT_YVU420M";
    }
    case V4L2_PIX_FMT_YUV422M:
    {
      return "V4L2_PIX_FMT_YUV422M";
    }
    case V4L2_PIX_FMT_YVU422M:
    {
      return "V4L2_PIX_FMT_YVU422M";
    }
    case V4L2_PIX_FMT_YUV444M:
    {
      return "V4L2_PIX_FMT_YUV444M";
    }
    case V4L2_PIX_FMT_YVU444M:
    {
      return "V4L2_PIX_FMT_YVU444M";
    }
    case V4L2_PIX_FMT_SBGGR8:
    {
      return "V4L2_PIX_FMT_SBGGR8";
    }
    case V4L2_PIX_FMT_SGBRG8:
    {
      return "V4L2_PIX_FMT_SGBRG8";
    }
    case V4L2_PIX_FMT_SGRBG8:
    {
      return "V4L2_PIX_FMT_SGRBG8";
    }
    case V4L2_PIX_FMT_SRGGB8:
    {
      return "V4L2_PIX_FMT_SRGGB8";
    }
    case V4L2_PIX_FMT_SBGGR10:
    {
      return "V4L2_PIX_FMT_SBGGR10";
    }
    case V4L2_PIX_FMT_SGBRG10:
    {
      return "V4L2_PIX_FMT_SGBRG10";
    }
    case V4L2_PIX_FMT_SGRBG10:
    {
      return "V4L2_PIX_FMT_SGRBG10";
    }
    case V4L2_PIX_FMT_SRGGB10:
    {
      return "V4L2_PIX_FMT_SRGGB10";
    }
    case V4L2_PIX_FMT_SBGGR10P:
    {
      return "V4L2_PIX_FMT_SBGGR10P";
    }
    case V4L2_PIX_FMT_SGBRG10P:
    {
      return "V4L2_PIX_FMT_SGBRG10P";
    }
    case V4L2_PIX_FMT_SGRBG10P:
    {
      return "V4L2_PIX_FMT_SGRBG10P";
    }
    case V4L2_PIX_FMT_SRGGB10P:
    {
      return "V4L2_PIX_FMT_SRGGB10P";
    }
    case V4L2_PIX_FMT_SBGGR10ALAW8:
    {
      return "V4L2_PIX_FMT_SBGGR10ALAW8";
    }
    case V4L2_PIX_FMT_SGBRG10ALAW8:
    {
      return "V4L2_PIX_FMT_SGBRG10ALAW8";
    }
    case V4L2_PIX_FMT_SGRBG10ALAW8:
    {
      return "V4L2_PIX_FMT_SGRBG10ALAW8";
    }
    case V4L2_PIX_FMT_SRGGB10ALAW8:
    {
      return "V4L2_PIX_FMT_SRGGB10ALAW8";
    }
    case V4L2_PIX_FMT_SBGGR10DPCM8:
    {
      return "V4L2_PIX_FMT_SBGGR10DPCM8";
    }
    case V4L2_PIX_FMT_SGBRG10DPCM8:
    {
      return "V4L2_PIX_FMT_SGBRG10DPCM8";
    }
    case V4L2_PIX_FMT_SGRBG10DPCM8:
    {
      return "V4L2_PIX_FMT_SGRBG10DPCM8";
    }
    case V4L2_PIX_FMT_SRGGB10DPCM8:
    {
      return "V4L2_PIX_FMT_SRGGB10DPCM8";
    }
    case V4L2_PIX_FMT_SBGGR12:
    {
      return "V4L2_PIX_FMT_SBGGR12";
    }
    case V4L2_PIX_FMT_SGBRG12:
    {
      return "V4L2_PIX_FMT_SGBRG12";
    }
    case V4L2_PIX_FMT_SGRBG12:
    {
      return "V4L2_PIX_FMT_SGRBG12";
    }
    case V4L2_PIX_FMT_SRGGB12:
    {
      return "V4L2_PIX_FMT_SRGGB12";
    }
    case V4L2_PIX_FMT_SBGGR12P:
    {
      return "V4L2_PIX_FMT_SBGGR12P";
    }
    case V4L2_PIX_FMT_SGBRG12P:
    {
      return "V4L2_PIX_FMT_SGBRG12P";
    }
    case V4L2_PIX_FMT_SGRBG12P:
    {
      return "V4L2_PIX_FMT_SGRBG12P";
    }
    case V4L2_PIX_FMT_SRGGB12P:
    {
      return "V4L2_PIX_FMT_SRGGB12P";
    }
    case V4L2_PIX_FMT_SBGGR16:
    {
      return "V4L2_PIX_FMT_SBGGR16";
    }
    case V4L2_PIX_FMT_SGBRG16:
    {
      return "V4L2_PIX_FMT_SGBRG16";
    }
    case V4L2_PIX_FMT_SGRBG16:
    {
      return "V4L2_PIX_FMT_SGRBG16";
    }
    case V4L2_PIX_FMT_SRGGB16:
    {
      return "V4L2_PIX_FMT_SRGGB16";
    }
    case V4L2_PIX_FMT_HSV24:
    {
      return "V4L2_PIX_FMT_HSV24";
    }
    case V4L2_PIX_FMT_HSV32:
    {
      return "V4L2_PIX_FMT_HSV32";
    }
    case V4L2_PIX_FMT_MJPEG:
    {
      return "V4L2_PIX_FMT_MJPEG";
    }
    case V4L2_PIX_FMT_JPEG:
    {
      return "V4L2_PIX_FMT_JPEG";
    }
    case V4L2_PIX_FMT_DV:
    {
      return "V4L2_PIX_FMT_DV";
    }
    case V4L2_PIX_FMT_MPEG:
    {
      return "V4L2_PIX_FMT_MPEG";
    }
    case V4L2_PIX_FMT_H264:
    {
      return "V4L2_PIX_FMT_H264";
    }
    case V4L2_PIX_FMT_H264_NO_SC:
    {
      return "V4L2_PIX_FMT_H264_NO_SC";
    }
    case V4L2_PIX_FMT_H264_MVC:
    {
      return "V4L2_PIX_FMT_H264_MVC";
    }
    case V4L2_PIX_FMT_H263:
    {
      return "V4L2_PIX_FMT_H263";
    }
    case V4L2_PIX_FMT_MPEG1:
    {
      return "V4L2_PIX_FMT_MPEG1";
    }
    case V4L2_PIX_FMT_MPEG2:
    {
      return "V4L2_PIX_FMT_MPEG2";
    }
    case V4L2_PIX_FMT_MPEG4:
    {
      return "V4L2_PIX_FMT_MPEG4";
    }
    case V4L2_PIX_FMT_XVID:
    {
      return "V4L2_PIX_FMT_XVID";
    }
    case V4L2_PIX_FMT_VC1_ANNEX_G:
    {
      return "V4L2_PIX_FMT_VC1_ANNEX_G";
    }
    case V4L2_PIX_FMT_VC1_ANNEX_L:
    {
      return "V4L2_PIX_FMT_VC1_ANNEX_L";
    }
    case V4L2_PIX_FMT_VP8:
    {
      return "V4L2_PIX_FMT_VP8";
    }
    case V4L2_PIX_FMT_VP9:
    {
      return "V4L2_PIX_FMT_VP9";
    }
    case V4L2_PIX_FMT_CPIA1:
    {
      return "V4L2_PIX_FMT_CPIA1";
    }
    case V4L2_PIX_FMT_WNVA:
    {
      return "V4L2_PIX_FMT_WNVA";
    }
    case V4L2_PIX_FMT_SN9C10X:
    {
      return "V4L2_PIX_FMT_SN9C10X";
    }
    case V4L2_PIX_FMT_SN9C20X_I420:
    {
      return "V4L2_PIX_FMT_SN9C20X_I420";
    }
    case V4L2_PIX_FMT_PWC1:
    {
      return "V4L2_PIX_FMT_PWC1";
    }
    case V4L2_PIX_FMT_PWC2:
    {
      return "V4L2_PIX_FMT_PWC2";
    }
    case V4L2_PIX_FMT_ET61X251:
    {
      return "V4L2_PIX_FMT_ET61X251";
    }
    case V4L2_PIX_FMT_SPCA501:
    {
      return "V4L2_PIX_FMT_SPCA501";
    }
    case V4L2_PIX_FMT_SPCA505:
    {
      return "V4L2_PIX_FMT_SPCA505";
    }
    case V4L2_PIX_FMT_SPCA508:
    {
      return "V4L2_PIX_FMT_SPCA508";
    }
    case V4L2_PIX_FMT_SPCA561:
    {
      return "V4L2_PIX_FMT_SPCA561";
    }
    case V4L2_PIX_FMT_PAC207:
    {
      return "V4L2_PIX_FMT_PAC207";
    }
    case V4L2_PIX_FMT_MR97310A:
    {
      return "V4L2_PIX_FMT_MR97310A";
    }
    case V4L2_PIX_FMT_JL2005BCD:
    {
      return "V4L2_PIX_FMT_JL2005BCD";
    }
    case V4L2_PIX_FMT_SN9C2028:
    {
      return "V4L2_PIX_FMT_SN9C2028";
    }
    case V4L2_PIX_FMT_SQ905C:
    {
      return "V4L2_PIX_FMT_SQ905C";
    }
    case V4L2_PIX_FMT_PJPG:
    {
      return "V4L2_PIX_FMT_PJPG";
    }
    case V4L2_PIX_FMT_OV511:
    {
      return "V4L2_PIX_FMT_OV511";
    }
    case V4L2_PIX_FMT_OV518:
    {
      return "V4L2_PIX_FMT_OV518";
    }
    case V4L2_PIX_FMT_STV0680:
    {
      return "V4L2_PIX_FMT_STV0680";
    }
    case V4L2_PIX_FMT_TM6000:
    {
      return "V4L2_PIX_FMT_TM6000";
    }
    case V4L2_PIX_FMT_CIT_YYVYUY:
    {
      return "V4L2_PIX_FMT_CIT_YYVYUY";
    }
    case V4L2_PIX_FMT_KONICA420:
    {
      return "V4L2_PIX_FMT_KONICA420";
    }
    case V4L2_PIX_FMT_JPGL:
    {
      return "V4L2_PIX_FMT_JPGL";
    }
    case V4L2_PIX_FMT_SE401:
    {
      return "V4L2_PIX_FMT_SE401";
    }
    case V4L2_PIX_FMT_S5C_UYVY_JPG:
    {
      return "V4L2_PIX_FMT_S5C_UYVY_JPG";
    }
    case V4L2_PIX_FMT_Y8I:
    {
      return "V4L2_PIX_FMT_Y8I";
    }
    case V4L2_PIX_FMT_Y12I:
    {
      return "V4L2_PIX_FMT_Y12I";
    }
    case V4L2_PIX_FMT_Z16:
    {
      return "V4L2_PIX_FMT_Z16";
    }
    case V4L2_PIX_FMT_MT21C:
    {
      return "V4L2_PIX_FMT_MT21C";
    }
    case V4L2_PIX_FMT_INZI:
    {
      return "V4L2_PIX_FMT_INZI";
    }
    default:
    {
      return "V4L2_PIX_FMT_XXXX";
    }
  }
}

//----------------------------------------------------------------------------
v4l2_pix_format vtkPlusV4L2VideoSource::StringToFormat(const std::string& format)
{
  if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_RGB332", format))
  {
    return V4L2_PIX_FMT_RGB332;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_RGB444", format))
  {
    return V4L2_PIX_FMT_RGB444;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_ARGB444", format))
  {
    return V4L2_PIX_FMT_ARGB444;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_XRGB444", format))
  {
    return V4L2_PIX_FMT_XRGB444;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_RGB555", format))
  {
    return V4L2_PIX_FMT_RGB555;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_ARGB555", format))
  {
    return V4L2_PIX_FMT_ARGB555;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_XRGB555", format))
  {
    return V4L2_PIX_FMT_XRGB555;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_RGB565", format))
  {
    return V4L2_PIX_FMT_RGB565;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_RGB555X", format))
  {
    return V4L2_PIX_FMT_RGB555X;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_ARGB555X", format))
  {
    return V4L2_PIX_FMT_ARGB555X;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_XRGB555X", format))
  {
    return V4L2_PIX_FMT_XRGB555X;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_RGB565X", format))
  {
    return V4L2_PIX_FMT_RGB565X;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_BGR666", format))
  {
    return V4L2_PIX_FMT_BGR666;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_BGR24", format))
  {
    return V4L2_PIX_FMT_BGR24;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_RGB24", format))
  {
    return V4L2_PIX_FMT_RGB24;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_BGR32", format))
  {
    return V4L2_PIX_FMT_BGR32;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_ABGR32", format))
  {
    return V4L2_PIX_FMT_ABGR32;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_XBGR32", format))
  {
    return V4L2_PIX_FMT_XBGR32;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_RGB32", format))
  {
    return V4L2_PIX_FMT_RGB32;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_ARGB32", format))
  {
    return V4L2_PIX_FMT_ARGB32;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_XRGB32", format))
  {
    return V4L2_PIX_FMT_XRGB32;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_GREY", format))
  {
    return V4L2_PIX_FMT_GREY;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Y4", format))
  {
    return V4L2_PIX_FMT_Y4;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Y6", format))
  {
    return V4L2_PIX_FMT_Y6;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Y10", format))
  {
    return V4L2_PIX_FMT_Y10;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Y12", format))
  {
    return V4L2_PIX_FMT_Y12;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Y16", format))
  {
    return V4L2_PIX_FMT_Y16;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Y16_BE", format))
  {
    return V4L2_PIX_FMT_Y16_BE;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Y10BPACK", format))
  {
    return V4L2_PIX_FMT_Y10BPACK;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_PAL8", format))
  {
    return V4L2_PIX_FMT_PAL8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_UV8", format))
  {
    return V4L2_PIX_FMT_UV8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUYV", format))
  {
    return V4L2_PIX_FMT_YUYV;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YYUV", format))
  {
    return V4L2_PIX_FMT_YYUV;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YVYU", format))
  {
    return V4L2_PIX_FMT_YVYU;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_UYVY", format))
  {
    return V4L2_PIX_FMT_UYVY;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_VYUY", format))
  {
    return V4L2_PIX_FMT_VYUY;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Y41P", format))
  {
    return V4L2_PIX_FMT_Y41P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV444", format))
  {
    return V4L2_PIX_FMT_YUV444;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV555", format))
  {
    return V4L2_PIX_FMT_YUV555;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV565", format))
  {
    return V4L2_PIX_FMT_YUV565;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV32", format))
  {
    return V4L2_PIX_FMT_YUV32;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_HI240", format))
  {
    return V4L2_PIX_FMT_HI240;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_HM12", format))
  {
    return V4L2_PIX_FMT_HM12;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_M420", format))
  {
    return V4L2_PIX_FMT_M420;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV12", format))
  {
    return V4L2_PIX_FMT_NV12;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV21", format))
  {
    return V4L2_PIX_FMT_NV21;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV16", format))
  {
    return V4L2_PIX_FMT_NV16;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV61", format))
  {
    return V4L2_PIX_FMT_NV61;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV24", format))
  {
    return V4L2_PIX_FMT_NV24;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV42", format))
  {
    return V4L2_PIX_FMT_NV42;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV12M", format))
  {
    return V4L2_PIX_FMT_NV12M;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV21M", format))
  {
    return V4L2_PIX_FMT_NV21M;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV16M", format))
  {
    return V4L2_PIX_FMT_NV16M;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV61M", format))
  {
    return V4L2_PIX_FMT_NV61M;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV12MT", format))
  {
    return V4L2_PIX_FMT_NV12MT;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_NV12MT_16X16", format))
  {
    return V4L2_PIX_FMT_NV12MT_16X16;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV410", format))
  {
    return V4L2_PIX_FMT_YUV410;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YVU410", format))
  {
    return V4L2_PIX_FMT_YVU410;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV411P", format))
  {
    return V4L2_PIX_FMT_YUV411P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV420", format))
  {
    return V4L2_PIX_FMT_YUV420;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YVU420", format))
  {
    return V4L2_PIX_FMT_YVU420;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV422P", format))
  {
    return V4L2_PIX_FMT_YUV422P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV420M", format))
  {
    return V4L2_PIX_FMT_YUV420M;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YVU420M", format))
  {
    return V4L2_PIX_FMT_YVU420M;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV422M", format))
  {
    return V4L2_PIX_FMT_YUV422M;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YVU422M", format))
  {
    return V4L2_PIX_FMT_YVU422M;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YUV444M", format))
  {
    return V4L2_PIX_FMT_YUV444M;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_YVU444M", format))
  {
    return V4L2_PIX_FMT_YVU444M;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SBGGR8", format))
  {
    return V4L2_PIX_FMT_SBGGR8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGBRG8", format))
  {
    return V4L2_PIX_FMT_SGBRG8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGRBG8", format))
  {
    return V4L2_PIX_FMT_SGRBG8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SRGGB8", format))
  {
    return V4L2_PIX_FMT_SRGGB8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SBGGR10", format))
  {
    return V4L2_PIX_FMT_SBGGR10;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGBRG10", format))
  {
    return V4L2_PIX_FMT_SGBRG10;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGRBG10", format))
  {
    return V4L2_PIX_FMT_SGRBG10;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SRGGB10", format))
  {
    return V4L2_PIX_FMT_SRGGB10;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SBGGR10P", format))
  {
    return V4L2_PIX_FMT_SBGGR10P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGBRG10P", format))
  {
    return V4L2_PIX_FMT_SGBRG10P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGRBG10P", format))
  {
    return V4L2_PIX_FMT_SGRBG10P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SRGGB10P", format))
  {
    return V4L2_PIX_FMT_SRGGB10P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SBGGR10ALAW8", format))
  {
    return V4L2_PIX_FMT_SBGGR10ALAW8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGBRG10ALAW8", format))
  {
    return V4L2_PIX_FMT_SGBRG10ALAW8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGRBG10ALAW8", format))
  {
    return V4L2_PIX_FMT_SGRBG10ALAW8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SRGGB10ALAW8", format))
  {
    return V4L2_PIX_FMT_SRGGB10ALAW8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SBGGR10DPCM8", format))
  {
    return V4L2_PIX_FMT_SBGGR10DPCM8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGBRG10DPCM8", format))
  {
    return V4L2_PIX_FMT_SGBRG10DPCM8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGRBG10DPCM8", format))
  {
    return V4L2_PIX_FMT_SGRBG10DPCM8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SRGGB10DPCM8", format))
  {
    return V4L2_PIX_FMT_SRGGB10DPCM8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SBGGR12", format))
  {
    return V4L2_PIX_FMT_SBGGR12;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGBRG12", format))
  {
    return V4L2_PIX_FMT_SGBRG12;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGRBG12", format))
  {
    return V4L2_PIX_FMT_SGRBG12;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SRGGB12", format))
  {
    return V4L2_PIX_FMT_SRGGB12;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SBGGR12P", format))
  {
    return V4L2_PIX_FMT_SBGGR12P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGBRG12P", format))
  {
    return V4L2_PIX_FMT_SGBRG12P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGRBG12P", format))
  {
    return V4L2_PIX_FMT_SGRBG12P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SRGGB12P", format))
  {
    return V4L2_PIX_FMT_SRGGB12P;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SBGGR16", format))
  {
    return V4L2_PIX_FMT_SBGGR16;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGBRG16", format))
  {
    return V4L2_PIX_FMT_SGBRG16;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SGRBG16", format))
  {
    return V4L2_PIX_FMT_SGRBG16;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SRGGB16", format))
  {
    return V4L2_PIX_FMT_SRGGB16;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_HSV24", format))
  {
    return V4L2_PIX_FMT_HSV24;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_HSV32", format))
  {
    return V4L2_PIX_FMT_HSV32;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_MJPEG", format))
  {
    return V4L2_PIX_FMT_MJPEG;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_JPEG", format))
  {
    return V4L2_PIX_FMT_JPEG;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_DV", format))
  {
    return V4L2_PIX_FMT_DV;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_MPEG", format))
  {
    return V4L2_PIX_FMT_MPEG;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_H264", format))
  {
    return V4L2_PIX_FMT_H264;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_H264_NO_SC", format))
  {
    return V4L2_PIX_FMT_H264_NO_SC;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_H264_MVC", format))
  {
    return V4L2_PIX_FMT_H264_MVC;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_H263", format))
  {
    return V4L2_PIX_FMT_H263;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_MPEG1", format))
  {
    return V4L2_PIX_FMT_MPEG1;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_MPEG2", format))
  {
    return V4L2_PIX_FMT_MPEG2;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_MPEG4", format))
  {
    return V4L2_PIX_FMT_MPEG4;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_XVID", format))
  {
    return V4L2_PIX_FMT_XVID;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_VC1_ANNEX_G", format))
  {
    return V4L2_PIX_FMT_VC1_ANNEX_G;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_VC1_ANNEX_L", format))
  {
    return V4L2_PIX_FMT_VC1_ANNEX_L;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_VP8", format))
  {
    return V4L2_PIX_FMT_VP8;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_VP9", format))
  {
    return V4L2_PIX_FMT_VP9;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_CPIA1", format))
  {
    return V4L2_PIX_FMT_CPIA1;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_WNVA", format))
  {
    return V4L2_PIX_FMT_WNVA;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SN9C10X", format))
  {
    return V4L2_PIX_FMT_SN9C10X;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SN9C20X_I420", format))
  {
    return V4L2_PIX_FMT_SN9C20X_I420;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_PWC1", format))
  {
    return V4L2_PIX_FMT_PWC1;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_PWC2", format))
  {
    return V4L2_PIX_FMT_PWC2;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_ET61X251", format))
  {
    return V4L2_PIX_FMT_ET61X251;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SPCA501", format))
  {
    return V4L2_PIX_FMT_SPCA501;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SPCA505", format))
  {
    return V4L2_PIX_FMT_SPCA505;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SPCA508", format))
  {
    return V4L2_PIX_FMT_SPCA508;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SPCA561", format))
  {
    return V4L2_PIX_FMT_SPCA561;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_PAC207", format))
  {
    return V4L2_PIX_FMT_PAC207;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_MR97310A", format))
  {
    return V4L2_PIX_FMT_MR97310A;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_JL2005BCD", format))
  {
    return V4L2_PIX_FMT_JL2005BCD;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SN9C2028", format))
  {
    return V4L2_PIX_FMT_SN9C2028;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SQ905C", format))
  {
    return V4L2_PIX_FMT_SQ905C;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_PJPG", format))
  {
    return V4L2_PIX_FMT_PJPG;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_OV511", format))
  {
    return V4L2_PIX_FMT_OV511;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_OV518", format))
  {
    return V4L2_PIX_FMT_OV518;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_STV0680", format))
  {
    return V4L2_PIX_FMT_STV0680;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_TM6000", format))
  {
    return V4L2_PIX_FMT_TM6000;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_CIT_YYVYUY", format))
  {
    return V4L2_PIX_FMT_CIT_YYVYUY;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_KONICA420", format))
  {
    return V4L2_PIX_FMT_KONICA420;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_JPGL", format))
  {
    return V4L2_PIX_FMT_JPGL;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_SE401", format))
  {
    return V4L2_PIX_FMT_SE401;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_S5C_UYVY_JPG", format))
  {
    return V4L2_PIX_FMT_S5C_UYVY_JPG;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Y8I", format))
  {
    return V4L2_PIX_FMT_Y8I;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Y12I", format))
  {
    return V4L2_PIX_FMT_Y12I;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_Z16", format))
  {
    return V4L2_PIX_FMT_Z16;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_MT21C", format))
  {
    return V4L2_PIX_FMT_MT21C;
  }
  else if (PlusCommon::IsEqualInsensitive("V4L2_PIX_FMT_INZI", format))
  {
    return V4L2_PIX_FMT_INZI;
  }
  else
  {
    return v4l2_fourcc('x', 'x', 'x', 'x');
  }
}

//----------------------------------------------------------------------------
std::string vtkPlusV4L2VideoSource::FieldOrderToString(v4l2_field field)
{
  switch(field)
  {
case V4L2_FIELD_ANY:
{
 return "V4L2_FIELD_ANY";
}
case V4L2_FIELD_NONE:
{
 return "V4L2_FIELD_NONE";
}
case V4L2_FIELD_TOP:
{
 return "V4L2_FIELD_TOP";
}
case V4L2_FIELD_BOTTOM:
{
 return "V4L2_FIELD_BOTTOM";
}
case V4L2_FIELD_INTERLACED:
{
 return "V4L2_FIELD_INTERLACED";
}
case V4L2_FIELD_SEQ_TB:
{
 return "V4L2_FIELD_SEQ_TB";
}
case V4L2_FIELD_SEQ_BT:
{
 return "V4L2_FIELD_SEQ_BT";
}
case V4L2_FIELD_ALTERNATE:
{
 return "V4L2_FIELD_ALTERNATE";
}
case V4L2_FIELD_INTERLACED_TB:
{
 return "V4L2_FIELD_INTERLACED_TB";
}
case V4L2_FIELD_INTERLACED_BT:
{
 return "V4L2_FIELD_INTERLACED_BT";
}
default:
{
      return "V4L2_FIELD_ANY";
}
}
}

//----------------------------------------------------------------------------
v4l2_field vtkPlusV4L2VideoSource::StringToFieldOrder(const std::string& field)
{
if(PlusCommon::IsEqualInsensitive("V4L2_FIELD_ANY", field))
{
return V4L2_FIELD_ANY;
}
else if(PlusCommon::IsEqualInsensitive("V4L2_FIELD_NONE", field))
{
return V4L2_FIELD_NONE;
}
else if(PlusCommon::IsEqualInsensitive("V4L2_FIELD_TOP", field))
{
return V4L2_FIELD_TOP;
}
else if(PlusCommon::IsEqualInsensitive("V4L2_FIELD_BOTTOM", field))
{
return V4L2_FIELD_BOTTOM;
}
else if(PlusCommon::IsEqualInsensitive("V4L2_FIELD_INTERLACED", field))
{
return V4L2_FIELD_INTERLACED;
}
else if(PlusCommon::IsEqualInsensitive("V4L2_FIELD_SEQ_TB", field))
{
return V4L2_FIELD_SEQ_TB;
}
else if(PlusCommon::IsEqualInsensitive("V4L2_FIELD_SEQ_BT", field))
{
return V4L2_FIELD_SEQ_BT;
}
else if(PlusCommon::IsEqualInsensitive("V4L2_FIELD_ALTERNATE", field))
{
return V4L2_FIELD_ALTERNATE;
}
else if(PlusCommon::IsEqualInsensitive("V4L2_FIELD_INTERLACED_TB", field))
{
return V4L2_FIELD_INTERLACED_TB;
}
else if(PlusCommon::IsEqualInsensitive("V4L2_FIELD_INTERLACED_BT", field))
{
return V4L2_FIELD_INTERLACED_BT;
}
else
{
return V4L2_FIELD_ANY;
}
}
