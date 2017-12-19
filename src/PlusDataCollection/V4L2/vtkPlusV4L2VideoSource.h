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
public:
  static vtkPlusV4L2VideoSource* New();
  vtkTypeMacro(vtkPlusV4L2VideoSource, vtkPlusDevice);
  virtual void PrintSelf(ostream& os, vtkIndent indent) VTK_OVERRIDE;

  /*! Read configuration from xml data */
  PlusStatus ReadConfiguration(vtkXMLDataElement* config);
  /*! Write configuration to xml data */
  PlusStatus WriteConfiguration(vtkXMLDataElement* config);

  /*! Is this device a tracker */
  bool IsTracker() const { return false; }

  /*! Get an update from the tracking system and push the new transforms to the tools. This function is called by the tracker thread.*/
  PlusStatus InternalUpdate();

  /*! Verify the device is correctly configured */
  virtual PlusStatus NotifyConfigured();

protected:
  vtkPlusV4L2VideoSource();
  ~vtkPlusV4L2VideoSource();

  virtual PlusStatus InternalConnect();
  virtual PlusStatus InternalDisconnect();

protected:

};

#endif // __vtkPlusOpenCVCaptureVideoSource_h