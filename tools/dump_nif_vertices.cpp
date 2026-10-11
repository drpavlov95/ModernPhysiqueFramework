#include <NifFile.hpp>

#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	if (argc != 2) {
		std::cerr << "usage: dump_nif_vertices <mesh.nif>\n";
		return 2;
	}

	nifly::NifFile file;
	const int result = file.Load(argv[1]);
	if (result != 0 || !file.IsValid()) {
		std::cerr << "could not load NIF (error " << result << ")\n";
		return 1;
	}

	const auto shapes = file.GetShapes();
	std::cout << "# shapes " << shapes.size() << "\n";
	std::cout << std::setprecision(9);
	for (auto* shape : shapes) {
		if (!shape) {
			continue;
		}
		std::cout << "shape_header " << shape->name.get() << ' ' << shape->GetBlockName() << "\n";
		const auto* data = file.GetGeometryData(shape);
		std::vector<nifly::Vector3> dynamicVertices;
		if (!data) {
			if (auto* triShape = dynamic_cast<nifly::BSTriShape*>(shape)) {
				dynamicVertices = triShape->UpdateRawVertices();
			}
		}
		const auto& vertices = data ? data->vertices : dynamicVertices;
		if (vertices.empty()) {
			for (const auto& path : file.GetExternalGeometryPathRefs(shape)) {
				std::cout << "external " << path.get() << "\n";
			}
			continue;
		}
		std::cout << "shape " << shape->name.get() << " " << vertices.size() << "\n";
		for (const auto& vertex : vertices) {
			std::cout << vertex.x << ' ' << vertex.y << ' ' << vertex.z << '\n';
		}
		std::vector<nifly::Triangle> triangles;
		if (shape->GetTriangles(triangles)) {
			std::cout << "triangles " << triangles.size() << "\n";
			for (const auto& tri : triangles) {
				std::cout << tri.p1 << ' ' << tri.p2 << ' ' << tri.p3 << '\n';
			}
		}
	}
	return 0;
}
