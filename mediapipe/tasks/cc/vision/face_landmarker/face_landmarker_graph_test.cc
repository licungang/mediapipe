/* Copyright 2023 The MediaPipe Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <optional>

#include "absl/flags/flag.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "mediapipe/framework/api2/builder.h"
#include "mediapipe/framework/api2/port.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/deps/file_path.h"
#include "mediapipe/framework/formats/classification.pb.h"
#include "mediapipe/framework/formats/image.h"
#include "mediapipe/framework/formats/landmark.pb.h"
#include "mediapipe/framework/formats/rect.pb.h"
#include "mediapipe/framework/packet.h"
#include "mediapipe/framework/port/file_helpers.h"
#include "mediapipe/framework/port/gmock.h"
#include "mediapipe/framework/port/gtest.h"
#include "mediapipe/tasks/cc/core/mediapipe_builtin_op_resolver.h"
#include "mediapipe/tasks/cc/core/proto/base_options.pb.h"
#include "mediapipe/tasks/cc/core/proto/external_file.pb.h"
#include "mediapipe/tasks/cc/core/task_runner.h"
#include "mediapipe/tasks/cc/vision/face_detector/proto/face_detector_graph_options.pb.h"
#include "mediapipe/tasks/cc/vision/face_geometry/proto/face_geometry.pb.h"
#include "mediapipe/tasks/cc/vision/face_landmarker/proto/face_landmarker_graph_options.pb.h"
#include "mediapipe/tasks/cc/vision/face_landmarker/proto/face_landmarks_detector_graph_options.pb.h"
#include "mediapipe/tasks/cc/vision/utils/image_utils.h"

namespace mediapipe {
namespace tasks {
namespace vision {
namespace face_landmarker {
namespace {

using ::file::Defaults;
using ::file::GetTextProto;
using ::mediapipe::api2::Input;
using ::mediapipe::api2::Output;
using ::mediapipe::api2::builder::Graph;
using ::mediapipe::api2::builder::Source;
using ::mediapipe::file::JoinPath;
using ::mediapipe::tasks::core::TaskRunner;
using ::mediapipe::tasks::vision::DecodeImageFromFile;
using ::mediapipe::tasks::vision::face_geometry::proto::FaceGeometry;
using ::mediapipe::tasks::vision::face_landmarker::proto::
    FaceLandmarkerGraphOptions;
using ::testing::EqualsProto;
using ::testing::Pointwise;
using ::testing::TestParamInfo;
using ::testing::TestWithParam;
using ::testing::Values;
using ::testing::proto::Approximately;
using ::testing::proto::Partially;

constexpr char kTestDataDirectory[] = "/mediapipe/tasks/testdata/vision/";
constexpr char kFaceLandmarkerModelBundleName[] = "face_landmarker_v2.task";
constexpr char kFaceLandmarkerWithBlendshapesModelBundleName[] =
    "face_landmarker_v2_with_blendshapes.task";
constexpr char kPortraitImageName[] = "portrait.jpg";
constexpr char kCatImageName[] = "cat.jpg";
constexpr char kPortraitExpectedFaceLandmarksName[] =
    "portrait_expected_face_landmarks.pbtxt";
constexpr char kPortraitExpectedBlendshapesName[] =
    "portrait_expected_blendshapes.pbtxt";
constexpr char kPortraitExpectedFaceGeometryName[] =
    "portrait_expected_face_geometry.pbtxt";

constexpr char kImageTag[] = "IMAGE";
constexpr char kImageName[] = "image";
constexpr char kNormRectTag[] = "NORM_RECT";
constexpr char kNormRectName[] = "norm_rect";
constexpr char kNormLandmarksTag[] = "NORM_LANDMARKS";
constexpr char kNormLandmarksName[] = "norm_landmarks";
constexpr char kBlendshapesTag[] = "BLENDSHAPES";
constexpr char kBlendshapesName[] = "blendshapes";
constexpr char kFaceGeometryTag[] = "FACE_GEOMETRY";
constexpr char kFaceGeometryName[] = "face_geometry";

constexpr float kLandmarksDiffMargin = 0.03;
constexpr float kBlendshapesDiffMargin = 0.1;
constexpr float kFaceGeometryDiffMargin = 0.02;

template <typename ProtoT>
ProtoT GetExpectedProto(absl::string_view filename) {
  ProtoT expected_proto;
  MP_EXPECT_OK(GetTextProto(file::JoinPath("./", kTestDataDirectory, filename),
                            &expected_proto, Defaults()));
  return expected_proto;
}

// Struct holding the parameters for parameterized FaceLandmarkerGraphTest
// class.
struct FaceLandmarkerGraphTestParams {
  // The name of this test, for convenience when displaying test results.
  std::string test_name;
  // The filename of the model to test.
  std::string input_model_name;
  // The filename of the test image.
  std::string test_image_name;
  // The expected output landmarks positions.
  std::optional<std::vector<NormalizedLandmarkList>> expected_landmarks_list;
  // The expected output blendshape classification.
  std::optional<std::vector<ClassificationList>> expected_blendshapes;
  // The expected output face geometry.
  std::optional<std::vector<FaceGeometry>> expected_face_geometry;
  // The max value difference between expected_positions and detected positions.
  float landmarks_diff_threshold;
  // The max value difference between expected blendshapes and actual
  // blendshapes.
  float blendshapes_diff_threshold;
  // The max value difference between expected blendshape and actual face
  // geometry.
  float face_geometry_diff_threshold;
};

// Helper function to create a FaceLandmarkerGraph TaskRunner.
absl::StatusOr<std::unique_ptr<TaskRunner>> CreateFaceLandmarkerGraphTaskRunner(
    absl::string_view model_name, bool output_blendshape,
    bool output_face_geometry) {
  Graph graph;

  auto& face_landmarker = graph.AddNode(
      "mediapipe.tasks.vision.face_landmarker."
      "FaceLandmarkerGraph");

  auto* options = &face_landmarker.GetOptions<FaceLandmarkerGraphOptions>();
  options->mutable_base_options()->mutable_model_asset()->set_file_name(
      JoinPath("./", kTestDataDirectory, model_name));
  options->mutable_face_detector_graph_options()->set_num_faces(1);
  options->mutable_base_options()->set_use_stream_mode(true);

  graph[Input<Image>(kImageTag)].SetName(kImageName) >>
      face_landmarker.In(kImageTag);
  graph[Input<NormalizedRect>(kNormRectTag)].SetName(kNormRectName) >>
      face_landmarker.In(kNormRectTag);

  face_landmarker.Out(kNormLandmarksTag).SetName(kNormLandmarksName) >>
      graph[Output<std::vector<NormalizedLandmarkList>>(kNormLandmarksTag)];
  if (output_blendshape) {
    face_landmarker.Out(kBlendshapesTag).SetName(kBlendshapesName) >>
        graph[Output<std::vector<ClassificationList>>(kBlendshapesTag)];
  }
  if (output_face_geometry) {
    face_landmarker.Out(kFaceGeometryTag).SetName(kFaceGeometryName) >>
        graph[Output<std::vector<FaceGeometry>>(kFaceGeometryTag)];
  }

  return TaskRunner::Create(
      graph.GetConfig(),
      absl::make_unique<tasks::core::MediaPipeBuiltinOpResolver>());
}

// Helper function to construct NormalizeRect proto.
NormalizedRect MakeNormRect(float x_center, float y_center, float width,
                            float height, float rotation) {
  NormalizedRect face_rect;
  face_rect.set_x_center(x_center);
  face_rect.set_y_center(y_center);
  face_rect.set_width(width);
  face_rect.set_height(height);
  face_rect.set_rotation(rotation);
  return face_rect;
}

class FaceLandmarkerGraphTest
    : public testing::TestWithParam<FaceLandmarkerGraphTestParams> {};

TEST(FaceLandmarkerGraphTest, FailsWithNoBlendshapesModel) {
  MP_ASSERT_OK_AND_ASSIGN(
      Image image, DecodeImageFromFile(
                       JoinPath("./", kTestDataDirectory, kPortraitImageName)));
  auto result =
      CreateFaceLandmarkerGraphTaskRunner(kFaceLandmarkerModelBundleName,
                                          /*output_blendshape=*/true,
                                          /*output_face_geometry=*/false);
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(result.status().message(),
              testing::HasSubstr(
                  "BLENDSHAPES Tag and blendshapes model must be both set."));
}

TEST_P(FaceLandmarkerGraphTest, Succeeds) {
  MP_ASSERT_OK_AND_ASSIGN(
      Image image, DecodeImageFromFile(JoinPath("./", kTestDataDirectory,
                                                GetParam().test_image_name)));
  MP_ASSERT_OK_AND_ASSIGN(auto task_runner,
                          CreateFaceLandmarkerGraphTaskRunner(
                              GetParam().input_model_name,
                              GetParam().expected_blendshapes.has_value(),
                              GetParam().expected_face_geometry.has_value()));

  auto output_packets = task_runner->Process(
      {{kImageName, MakePacket<Image>(std::move(image))},
       {kNormRectName,
        MakePacket<NormalizedRect>(MakeNormRect(0.5, 0.5, 1.0, 1.0, 0))}});
  MP_ASSERT_OK(output_packets);

  if (GetParam().expected_landmarks_list) {
    const std::vector<NormalizedLandmarkList>& landmarks_lists =
        (*output_packets)[kNormLandmarksName]
            .Get<std::vector<NormalizedLandmarkList>>();
    EXPECT_THAT(landmarks_lists,
                Pointwise(Approximately(Partially(EqualsProto()),
                                        GetParam().landmarks_diff_threshold),
                          *GetParam().expected_landmarks_list));
  }

  if (GetParam().expected_blendshapes) {
    const std::vector<ClassificationList>& blendshapes =
        (*output_packets)[kBlendshapesName]
            .Get<std::vector<ClassificationList>>();
    EXPECT_THAT(blendshapes,
                Pointwise(Approximately(Partially(EqualsProto()),
                                        GetParam().blendshapes_diff_threshold),
                          *GetParam().expected_blendshapes));
  }
  if (GetParam().expected_face_geometry) {
    const std::vector<FaceGeometry>& face_geometry =
        (*output_packets)[kFaceGeometryName].Get<std::vector<FaceGeometry>>();
    EXPECT_THAT(
        face_geometry,
        Pointwise(Approximately(Partially(EqualsProto()),
                                GetParam().face_geometry_diff_threshold),
                  *GetParam().expected_face_geometry));
  }
}

INSTANTIATE_TEST_SUITE_P(
    FaceLandmarkerGraphTests, FaceLandmarkerGraphTest,
    Values(FaceLandmarkerGraphTestParams{
               /* test_name= */ "Portrait",
               /* input_model_name= */ kFaceLandmarkerModelBundleName,
               /* test_image_name= */ kPortraitImageName,
               /* expected_landmarks_list= */
               {{GetExpectedProto<NormalizedLandmarkList>(
                   kPortraitExpectedFaceLandmarksName)}},
               /* expected_blendshapes= */ std::nullopt,
               /* expected_face_geometry= */ std::nullopt,
               /* landmarks_diff_threshold= */ kLandmarksDiffMargin,
               /* blendshapes_diff_threshold= */ kBlendshapesDiffMargin,
               /* face_geometry_diff_threshold= */
               kFaceGeometryDiffMargin},
           FaceLandmarkerGraphTestParams{
               /* test_name= */ "NoFace",
               /* input_model_name= */ kFaceLandmarkerModelBundleName,
               /* test_image_name= */ kCatImageName,
               /* expected_landmarks_list= */ std::nullopt,
               /* expected_blendshapes= */ std::nullopt,
               /* expected_face_geometry= */ std::nullopt,
               /* landmarks_diff_threshold= */ kLandmarksDiffMargin,
               /* blendshapes_diff_threshold= */ kBlendshapesDiffMargin,
               /* face_geometry_diff_threshold= */
               kFaceGeometryDiffMargin},
           FaceLandmarkerGraphTestParams{
               /* test_name= */ "PortraitWithBlendshape",
               /* input_model_name= */
               kFaceLandmarkerWithBlendshapesModelBundleName,
               /* test_image_name= */ kPortraitImageName,
               /* expected_landmarks_list= */
               {{GetExpectedProto<NormalizedLandmarkList>(
                   kPortraitExpectedFaceLandmarksName)}},
               /* expected_blendshapes= */
               {{GetExpectedProto<ClassificationList>(
                   kPortraitExpectedBlendshapesName)}},
               /* expected_face_geometry= */ std::nullopt,
               /*landmarks_diff_threshold= */ kLandmarksDiffMargin,
               /*blendshapes_diff_threshold= */ kBlendshapesDiffMargin,
               /*face_geometry_diff_threshold= */ kFaceGeometryDiffMargin},
           FaceLandmarkerGraphTestParams{
               /* test_name= */ "PortraitWithBlendshapeWithFaceGeometry",
               /* input_model_name= */
               kFaceLandmarkerWithBlendshapesModelBundleName,
               /* test_image_name= */ kPortraitImageName,
               /* expected_landmarks_list= */
               {{GetExpectedProto<NormalizedLandmarkList>(
                   kPortraitExpectedFaceLandmarksName)}},
               /* expected_blendshapes= */
               {{GetExpectedProto<ClassificationList>(
                   kPortraitExpectedBlendshapesName)}},
               /* expected_face_geometry= */
               {{GetExpectedProto<FaceGeometry>(
                   kPortraitExpectedFaceGeometryName)}},
               /*landmarks_diff_threshold= */ kLandmarksDiffMargin,
               /*blendshapes_diff_threshold= */ kBlendshapesDiffMargin,
               /*face_geometry_diff_threshold= */ kFaceGeometryDiffMargin}),
    [](const TestParamInfo<FaceLandmarkerGraphTest::ParamType>& info) {
      return info.param.test_name;
    });

}  // namespace
}  // namespace face_landmarker
}  // namespace vision
}  // namespace tasks
}  // namespace mediapipe
