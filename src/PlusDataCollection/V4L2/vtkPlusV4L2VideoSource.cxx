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

//----------------------------------------------------------------------------

vtkStandardNewMacro(vtkPlusV4L2VideoSource);

//----------------------------------------------------------------------------
vtkPlusV4L2VideoSource::vtkPlusV4L2VideoSource()
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

}

//-----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::ReadConfiguration(vtkXMLDataElement* rootConfigElement)
{
  LOG_TRACE("vtkPlusV4L2VideoSource::ReadConfiguration");
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_READING(deviceConfig, rootConfigElement);

  return PLUS_SUCCESS;
}

//-----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::WriteConfiguration(vtkXMLDataElement* rootConfigElement)
{
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_WRITING(deviceConfig, rootConfigElement);

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InternalConnect()
{

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InternalDisconnect()
{

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusV4L2VideoSource::InternalUpdate()
{
  LOG_TRACE("vtkPlusV4L2VideoSource::InternalUpdate");

  this->FrameNumber++;

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