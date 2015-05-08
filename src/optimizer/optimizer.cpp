#include "optimizer.hpp"

#include <iostream>

using std::vector;

using cv::Mat;
using cv::Range;
using cv::Size;

static const int CROP_ANGLE = 120;
static const int H_FOCUS_ANGLE = 20;
static const int V_FOCUS_ANGLE = 20;
static const int BLUR_FACTOR = 5;

OptimizedImage::OptimizedImage(const Mat& focused,
    const Mat& blurredLeft,
    const Mat& blurredRight,
    const Mat& blurredTop,
    const Mat& blurredBottom,
    Size origHSize,
    Size origVSize,
    Size fullSize,
    int leftBuffer) {
  this->focused = Mat(focused);
  this->blurredLeft = Mat(blurredLeft);
  this->blurredRight = Mat(blurredRight);
  this->blurredTop = Mat(blurredTop);
  this->blurredBottom = Mat(blurredBottom);
  this->origHSize = origHSize;
  this->origVSize = origVSize;
  this->fullSize = fullSize;
  this->leftBuffer = leftBuffer;
}

size_t OptimizedImage::size() const {
  return ImageUtil::imageSize(focused) +
    ImageUtil::imageSize(blurredLeft) +
    ImageUtil::imageSize(blurredRight) +
    ImageUtil::imageSize(blurredTop) +
    ImageUtil::imageSize(blurredBottom);
}

static inline int constrainAngle(int x){
  x %= 360;
  if (x < 0)
    x += 360;
  ENSURES(0 <= x && x < 360);
  return x;
}

static inline double constrainAngle(double x){
  // https://stackoverflow.com/questions/11498169/dealing-with-angle-wrap-in-c-code
  x = fmod(x,360);
  if (x < 0)
    x += 360;
  ENSURES(0 <= x && x < 360);
  return x;
}

OptimizedImage Optimizer::optimizeImage(const Mat& image,
    int angle, int vAngle) {
  double start, end;

  angle = constrainAngle(angle);
  REQUIRES(H_FOCUS_ANGLE < CROP_ANGLE);

  const int width = image.cols;
  const int height = image.rows;
  const double angleToWidth = width / 360.0;
  const double angleToHeight = height / 180.0;

  int leftAngle = constrainAngle(angle - CROP_ANGLE / 2);
  int rightAngle = constrainAngle(angle + CROP_ANGLE / 2);

  int leftCol = leftAngle * angleToWidth;
  int rightCol = rightAngle * angleToWidth;
  ASSERT(0 <= leftCol);
  ASSERT(leftCol < width);
  ASSERT(0 <= rightCol);
  ASSERT(rightCol < width);

  start = Timer::time();
  Mat cropped;
  if (leftCol < rightCol) {
    // Cropped window doesn't wrap around, simple case.
    cropped = Mat(image, Range::all(), Range(leftCol, rightCol));
  } else {
    // Cropped window *does* wrap around. We need to get the part
    // before and after it wraps, which are in two separate places
    // on the image.
    Mat leftMat = Mat(image, Range::all(), Range(leftCol, width));
    Mat rightMat = Mat(image, Range::all(), Range(0, rightCol));
    hconcat(leftMat, rightMat, cropped);
  }
  end = Timer::time();
  std::cout << "Cropping: " << end - start << " ms" << std::endl;

  int focusWidth = H_FOCUS_ANGLE * angleToWidth;

  int focusLeftCol = cropped.cols / 2 - focusWidth / 2;
  int focusRightCol = cropped.cols / 2 + focusWidth / 2;

  start = Timer::time();
  ASSERT(0 <= focusLeftCol);
  ASSERT(focusLeftCol <= focusRightCol);
  ASSERT(focusRightCol < cropped.cols);
  Mat middle = Mat(cropped, Range::all(), Range(focusLeftCol, focusRightCol));
  Mat left = Mat(cropped, Range::all(), Range(0, focusLeftCol));
  Mat right = Mat(cropped, Range::all(), Range(focusRightCol, cropped.cols));
  end = Timer::time();
  std::cout << "Splitting (H): " << end - start << " ms" << std::endl;

  Size origHSize(left.size());

  Mat blurredLeft = left;
  Mat blurredRight = right;

  start = Timer::time();
  Size smallSize(left.cols / BLUR_FACTOR, left.rows / BLUR_FACTOR);
  cv::resize(left, blurredLeft, smallSize);
  cv::resize(right, blurredRight, smallSize);
  end = Timer::time();
  std::cout << "Blurring (H): " << end - start << " ms" << std::endl;

  int focusHeight = V_FOCUS_ANGLE * angleToHeight;
  int focusMiddleRow = vAngle * angleToHeight;
  int focusTopRow = focusMiddleRow - focusHeight / 2;
  int focusBottomRow = focusMiddleRow + focusHeight / 2;

  start = Timer::time();
  Mat top(middle, Range(0, focusTopRow));
  Mat focused(middle, Range(focusTopRow, focusBottomRow));
  Mat bottom(middle, Range(focusBottomRow, middle.rows));
  end = Timer::time();
  std::cout << "Splitting (V): " << end - start << " ms" << std::endl;

  start = Timer::time();
  Mat blurredTop, blurredBottom;
  Size origVSize(top.size());
  Size smallVSize(top.cols / BLUR_FACTOR, top.rows / BLUR_FACTOR);
  cv::resize(top, blurredTop, smallVSize);
  cv::resize(bottom, blurredBottom, smallVSize);
  end = Timer::time();
  std::cout << "Blurring (V): " << end - start << " ms" << std::endl;

  OptimizedImage optImage(focused,
      blurredLeft, blurredRight,
      blurredTop, blurredBottom,
      origHSize, origVSize,
      image.size(), leftCol);
  return optImage;
}

Mat Optimizer::extractImage(const OptimizedImage& optImage) {
  double start, end;
  Mat left, right, top, bottom;

  start = Timer::time();
  cv::resize(optImage.blurredLeft, left, optImage.origHSize);
  cv::resize(optImage.blurredRight, right, optImage.origHSize);
  end = Timer::time();
  std::cout << "Resizing (H): " << end - start << " ms" << std::endl;

  start = Timer::time();
  cv::resize(optImage.blurredTop, top, optImage.origVSize);
  cv::resize(optImage.blurredBottom, bottom, optImage.origVSize);
  end = Timer::time();
  std::cout << "Resizing (V): " << end - start << " ms" << std::endl;

  start = Timer::time();
  // Reconstruct middle image
  Mat middle;
  ImageUtil::vconcat3(top, optImage.focused, bottom, middle);

  // Reconstruct cropped image
  Mat croppedImage;
  ImageUtil::hconcat3(left, middle, right, croppedImage);
  end = Timer::time();
  std::cout << "Reconstructing: " << end - start << " ms" << std::endl;

  Mat fullLeft, fullCenter, fullRight;

  int numRows = croppedImage.size().height;

  int origType = optImage.focused.type();

  start = Timer::time();
  // Split into 2 cases. In either case, there are 3 images to create
  if (croppedImage.size().width + optImage.leftBuffer >= optImage.fullSize.width) {
    // cropped image wraps around
    // Reconstruct as cropped_right + black + cropped_left

    // cropped right = leftBuffer to end, or in local coords, 0 to width - leftBuffer?
    // cropped left =
    // black = full width - right - left

    int rightEndExclusive = optImage.fullSize.width - optImage.leftBuffer;
    ASSERT(0 <= rightEndExclusive && rightEndExclusive <= croppedImage.cols);
    fullRight = Mat(croppedImage, Range::all(), Range(0, rightEndExclusive));
    fullLeft = Mat(croppedImage, Range::all(), Range(rightEndExclusive, croppedImage.cols));

    int centerCols = optImage.fullSize.width - fullRight.size().width - fullLeft.size().width;
    ASSERT(centerCols >= 0);

    fullCenter = Mat(numRows, centerCols, origType);
    fullCenter.setTo(cv::Scalar(0,0,0));

  } else {
    // cropped image fully contained
    // Reconstruct as black_left + cropped + black_right

    int leftBufferCols = optImage.leftBuffer; //(optImage.fullSize.width - croppedImage.size().width)/2;
    fullLeft = Mat(numRows, leftBufferCols, origType);
    fullCenter = croppedImage;

    int rightBufferCols = optImage.fullSize.width - leftBufferCols - croppedImage.size().width;
    ASSERT(rightBufferCols >= 0);

    fullRight = Mat(numRows, rightBufferCols, origType);

    fullLeft.setTo(cv::Scalar(0,0,0));
    fullRight.setTo(cv::Scalar(0,0,0));
  }


  Mat fullImage;
  ImageUtil::hconcat3(fullLeft, fullCenter, fullRight, fullImage);
  end = Timer::time();
  std::cout << "Full image: " << end - start << " ms" << std::endl;

  ENSURES(fullImage.size() == optImage.fullSize);

  return fullImage;
}