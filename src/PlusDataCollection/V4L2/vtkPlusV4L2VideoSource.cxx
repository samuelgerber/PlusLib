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

vtkStandardNewMacro (vtkPlusV4L2VideoSource);

//----------------------------------------------------------------------------

namespace
{
  int xioctl(int fh, unsigned long int request, void *arg)
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

  if(this->FileDescriptor != -1)
  {
    os << indent << "Available formats: " << std::endl;

    v4l2_fmtdesc fmtdesc;
    CLEAR(fmtdesc);
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (ioctl(this->FileDescriptor,VIDIOC_ENUM_FMT,&fmtdesc) == 0)
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
  if(vtkPlusV4L2VideoSource::StringToIOMethod(ioMethod) != IO_METHOD_UNKNOWN)
  {
    this->IOMethod = vtkPlusV4L2VideoSource::StringToIOMethod(ioMethod);
  }
  else
  {
    LOG_WARNING("Unknown method: " << ioMethod << ". Defaulting to " << vtkPlusV4L2VideoSource::IOMethodToString(this->IOMethod));
  }

  int frameSize[3];
  XML_READ_VECTOR_ATTRIBUTE_NONMEMBER_OPTIONAL(int, 3, FrameSize, frameSize, deviceConfig);
  if(deviceConfig->GetAttribute("FrameSize") != nullptr)
  {
    this->RequestedFormat->fmt.pix.width = frameSize[0];
    this->RequestedFormat->fmt.pix.height = frameSize[1];
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
