# Fails the build when the app image is larger than MAX_SIZE bytes.
#
#   cmake -DIMAGE=<path> -DMAX_SIZE=<bytes> -P check-image-size.cmake

file(SIZE "${IMAGE}" IMAGE_SIZE)

math(EXPR PERCENT "${IMAGE_SIZE} * 100 / ${MAX_SIZE}")

if(IMAGE_SIZE GREATER MAX_SIZE)
    message(FATAL_ERROR "${IMAGE} is ${IMAGE_SIZE} bytes, over the limit of ${MAX_SIZE} bytes")
endif()

message(STATUS "${IMAGE} is ${IMAGE_SIZE} bytes, ${PERCENT}% of the limit of ${MAX_SIZE} bytes")
