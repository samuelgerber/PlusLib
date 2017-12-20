/*=Plus=header=begin======================================================
 Program: Plus
 Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
 See License.txt for details.
 =========================================================Plus=header=end*/

#ifndef __vtkPlusOpenCVCaptureVideoSource_h
#define __vtkPlusOpenCVCaptureVideoSource_h

#include "vtkPlusDataCollectionExport.h"
#include "vtkPlusDevice.h"

/*!
 \class vtkPlusV4L2VideoSource
 \brief Class for interfacing an V4L2 device and recording frames into a Plus buffer

 Requires the PLUS_USE_V4L2 option in CMake.

 \ingroup PlusLibDataCollection
 */

class vtkPlusDataCollectionExport vtkPlusV4L2VideoSource : public vtkPlusDevice
{
  enum V4L2_IO_METHOD
  {
    IO_METHOD_READ,
    IO_METHOD_MMAP,
    IO_METHOD_USERPTR,
  };

  struct FrameBuffer
  {
    void *start;
    size_t length;
  };

  public:
  static vtkPlusV4L2VideoSource* New();
  vtkTypeMacro(vtkPlusV4L2VideoSource, vtkPlusDevice);
  virtual void PrintSelf(ostream& os, vtkIndent indent) VTK_OVERRIDE;

  /*! Read configuration from xml data */
  PlusStatus ReadConfiguration(vtkXMLDataElement* config);
  /*! Write configuration to xml data */
  PlusStatus WriteConfiguration(vtkXMLDataElement* config);

  /*! Is this device a tracker */
  bool IsTracker() const
  { return false;}

  /*! Poll the device for new frames */
  PlusStatus InternalUpdate();

  /*! Verify the device is correctly configured */
  virtual PlusStatus NotifyConfigured();

  vtkSetStdStringMacro(DeviceName);
  vtkGetStdStringMacro(DeviceName);

  protected:
  vtkPlusV4L2VideoSource();
  ~vtkPlusV4L2VideoSource();

  PlusStatus ReadFrame();
  PlusStatus InitRead(unsigned int bufferSize);
  PlusStatus InitMmap();
  PlusStatus InitUserp(unsigned int bufferSize);

  virtual PlusStatus InternalConnect() VTK_OVERRIDE;
  virtual PlusStatus InternalDisconnect() VTK_OVERRIDE;

  virtual PlusStatus InternalStopRecording() VTK_OVERRIDE;
  virtual PlusStatus InternalStartRecording() VTK_OVERRIDE;

  protected:
  std::string DeviceName;
  V4L2_IO_METHOD IOMethod = IO_METHOD_MMAP;
  int FileDescriptor = -1;
  FrameBuffer* Frames = nullptr;
  unsigned int BufferCount = 0;
  bool ForceFormat = false;
};

#endif // __vtkPlusOpenCVCaptureVideoSource_h
