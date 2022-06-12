from cslam.third_party.mac.mac import MAC
from cslam.third_party.mac.utils import Edge, weight_graph_lap_from_edge_list
import numpy as np
from typing import NamedTuple


class EdgeInterRobot(NamedTuple):
    """ Inter-robot loop closure edge
    """
    robot0_id: int
    robot0_image_id: int
    robot1_id: int
    robot1_image_id: int
    weight: float

    def __eq__(self, other):
        """ Overload the equal operator in order to ignore the weights

        Args:
            other (EdgeInterRobot): Other edge to compare
        """
        return ((self.robot0_id == other.robot0_id)
                and (self.robot0_image_id == other.robot0_image_id)
                and (self.robot1_id == other.robot1_id)
                and (self.robot1_image_id == other.robot1_image_id))


class AlgebraicConnectivityMaximization(object):

    def __init__(self,
                 robot_id=0,
                 nb_robots=1,
                 max_iters=20,
                 fixed_weight=1.0):
        """Initialization

        Args:
            robot_id (int, optional): ID of the robot
            nb_robots (int, optional): number of robots. Defaults to 1.
            max_iters (int, optional): maximum number of iterations. Defaults to 20.
            fixed_weight (float, optional): weight of fixed measurements. Defaults to 1.0.
        """
        self.fixed_weight = fixed_weight

        self.fixed_edges = []
        self.candidate_edges = {}

        self.max_iters = max_iters

        self.nb_robots = nb_robots
        self.robot_id = robot_id
        self.total_nb_poses = 0

        self.nb_poses = {}
        for i in range(self.nb_robots):
            self.nb_poses[i] = 0

    def set_graph(self, fixed_edges, candidate_edges):
        """Fill graph struct

        Args:
            fixed_edges (list(EdgeInterRobot)): edges that are already computed
            candidate_edges (list(EdgeInterRobot)): candidate edges to compute
        """
        self.fixed_edges = fixed_edges

        # Extract nb of poses for ids graphs
        for e in self.fixed_edges:
            self.nb_poses[e.robot0_id] = max(self.nb_poses[e.robot0_id],
                                             e.robot0_image_id + 1)
            self.nb_poses[e.robot1_id] = max(self.nb_poses[e.robot1_id],
                                             e.robot1_image_id + 1)
        for e in candidate_edges:
            self.nb_poses[e.robot0_id] = max(self.nb_poses[e.robot0_id],
                                             e.robot0_image_id + 1)
            self.nb_poses[e.robot1_id] = max(self.nb_poses[e.robot1_id],
                                             e.robot1_image_id + 1)

        for e in candidate_edges:
            self.candidate_edges[(e.robot0_image_id, e.robot1_id)] = e

    def add_fixed_edge(self, edge):
        """Add an already computed edge to the graph

        Args:
            edge (EdgeInterRobot): inter-robot edge
        """
        self.fixed_edges.append(edge)
        # Update nb of poses
        self.nb_poses[edge.robot0_id] = max(self.nb_poses[edge.robot0_id],
                                            edge.robot0_image_id + 1)
        self.nb_poses[edge.robot1_id] = max(self.nb_poses[edge.robot1_id],
                                            edge.robot1_image_id + 1)

    def add_candidate_edge(self, edge):
        """Add a candidate edge to the graph

        Args:
            edge (EdgeInterRobot): inter-robot edge
        """
        self.candidate_edges[(edge.robot0_image_id, edge.robot1_id)] = edge
        # Update nb of poses
        self.nb_poses[edge.robot0_id] = max(self.nb_poses[edge.robot0_id],
                                            edge.robot0_image_id + 1)
        self.nb_poses[edge.robot1_id] = max(self.nb_poses[edge.robot1_id],
                                            edge.robot1_image_id + 1)

    def remove_candidate_edges(self, edges):
        """Remove candidate edge from the graph

        Args:
            edges (list(EdgeInterRobot)): inter-robot edges
        """
        keys = list(self.candidate_edges.keys())
        for k in keys:
            if self.candidate_edges[k] in edges:
                del self.candidate_edges[k]

    def candidate_edges_to_fixed(self, edges):
        """Move candidate edges to fixed. 
        Use when candidates are successfully converted into measurements

        Args:
            edges (list(EdgeInterRobot)): inter-robot edges
        """
        for i in range(len(edges)):
            edges[i] = edges[i]._replace(weight=self.fixed_weight)
        self.fixed_edges.extend(edges)
        self.remove_candidate_edges(edges)

    def greedy_initialization(self, nb_candidates_to_choose, edges):
        """Greedy weight initialization

        Args:
            nb_candidates_to_choose (int): number of edges to choose
            edges (list(Edge)): candidate_edges
        """
        weights = [e.weight for e in edges]
        w_init = np.zeros(len(weights))
        indices = np.argpartition(
            weights, -nb_candidates_to_choose)[-nb_candidates_to_choose:]
        w_init[indices] = 1.0
        return w_init

    def pseudo_greedy_initialization(self, nb_candidates_to_choose, 
                                            nb_random,
                                            edges):
        """Greedy weight initialization
            Greedy initialization with for the first nb_candidates_to_choose-nb_random
            then random choice

        Args:
            nb_candidates_to_choose (int): number of edges to choose
            nb_random (int): number of edges to choose randomly
            edges (list(Edge)): candidate_edges
        """
        nb_greedy = nb_candidates_to_choose - nb_random
        w_init = self.greedy_initialization(nb_greedy, edges)
        nb_edges = len(edges)
        i = 0
        while i < nb_random:
            j = int(np.random.rand() * nb_edges)
            if w_init[j] < 0.5:
                w_init[j] = 1.0
                i = i + 1
        return w_init

    def random_initialization(self, nb_candidates_to_choose, edges):
        """Random weight initialization

        Args:
            nb_candidates_to_choose (int): number of edges to choose
        """
        for e in range(len(edges)):
            edges[e] = edges[e]._replace(weight=np.random.rand())
        return self.greedy_initialization(nb_candidates_to_choose, edges)

    def compute_offsets(self, is_robot_included):
        """Compute rekey offsets

        Args:
            is_robot_included dict(int, bool): Indicates if the robot 
                                is connected and in communication range
        """
        # Offsets required to put rekey nodes such
        # that they are all in a single graph
        self.offsets = {}
        for i in range(self.nb_robots):
            self.offsets[i] = 0
        # Compute offsets
        previous_offset = 0
        previous_nb_poses = 0
        for id in range(self.nb_robots):
            if is_robot_included[id]:
                self.offsets[id] = previous_offset + previous_nb_poses
                previous_offset = self.offsets[id]
                previous_nb_poses = self.nb_poses[id]

    def rekey_edges(self, edges, is_robot_included):
        """Modify keys (nodes ID) from robot_id+image_id to node_id
        Result example: 3 robots with 10 nodes eachs
        robot 0 nodes id = 1 to 9
        robot 1 nodes id = 10 to 19
        robot 2 nodes id = 20 to 29

        Args:
            edges (dict(EdgeInterRobot)): inter-robot edges
            is_robot_included dict(int, bool): Indicates if the robot 
                                is connected and in communication range

        Returns:
            list(Edge): edges with keys for MAC problem
        """
        # Rekey edges
        rekeyed_edges = []
        for e in edges:
            if is_robot_included[e.robot0_id] and is_robot_included[
                    e.robot1_id]:
                i = self.offsets[e.robot0_id] + e.robot0_image_id
                j = self.offsets[e.robot1_id] + e.robot1_image_id
                rekeyed_edges.append(Edge(i, j, e.weight))
        return rekeyed_edges

    def fill_odometry(self):
        """Add odometry edges
        We can infer the odometry edges directly from the number of poses,
        without communication.

        Returns:
            list(Edge): odometry edges
        """
        odom_edges = []
        for i in range(len(self.nb_poses)):
            for k in range(self.nb_poses[i] - 1):
                odom_edges.append(
                    Edge(self.offsets[i] + k, self.offsets[i] + k + 1,
                         self.fixed_weight))
        return odom_edges

    def recover_inter_robot_edges(self, edges):
        """Recover IDs from before rekey_edges()

        Args:
            edges (list(Edge)): rekeyed edges

        Returns:
            list(EdgeInterRobot): edges
        """
        recovered_inter_robot_edges = []
        for c in range(len(edges)):
            robot0_id = 0
            robot1_id = 0
            for o in range(len(self.offsets)):
                if o != 0:
                    if edges[c].i >= self.offsets[o]:
                        robot0_id = robot0_id + 1
                    if edges[c].j >= self.offsets[o]:
                        robot1_id = robot1_id + 1
            robot0_image_id = edges[c].i - self.offsets[robot0_id]
            robot1_image_id = edges[c].j - self.offsets[robot1_id]
            recovered_inter_robot_edges.append(
                EdgeInterRobot(robot0_id, robot0_image_id, robot1_id,
                               robot1_image_id, edges[c].weight))
        return recovered_inter_robot_edges

    def check_graph_connectivity(self):
        """Check if the current graph of potential matches is connected
        
        Returns:
            dict(int, bool): dict indicating if each robot is connected
            bool: true if at least one robot is connected
        """
        is_robot_connected = {}
        for i in range(self.nb_robots):
            if i == self.robot_id:
                is_robot_connected[i] = True
            else:
                is_robot_connected[i] = False
        for edge in self.candidate_edges.values():
            is_robot_connected[edge.robot0_id] = True
            is_robot_connected[edge.robot1_id] = True
        return is_robot_connected

    def select_candidates(self,
                          nb_candidates_to_choose,
                          greedy_initialization=True):
        """Solve algebraic connectivity maximization

        Args:
            nb_candidates_to_choose (int): number of candidates to choose,
                            related to a computation/communication budget
            greedy_initialization: perform greedy initialization based on similarity

        Returns:
            list(EdgeInterRobot): selected edges
        """
        # Check connectivity
        is_robot_included = self.check_graph_connectivity()
        # TODO: check if robots are in range

        # Rekey multi-robot edges to single robot
        self.compute_offsets(is_robot_included)
        rekeyed_fixed_edges = self.rekey_edges(self.fixed_edges,
                                               is_robot_included)
        rekeyed_fixed_edges.extend(self.fill_odometry())
        rekeyed_candidate_edges = self.rekey_edges(
            self.candidate_edges.values(), is_robot_included)

        if len(rekeyed_candidate_edges) > 0:
            # Compute number of poses
            self.total_nb_poses = 0
            for n in range(len(self.nb_poses)):
                self.total_nb_poses = self.total_nb_poses + self.nb_poses[n]

            # Initial guess
            if greedy_initialization is False:
                w_init = self.random_initialization(nb_candidates_to_choose,
                                                    rekeyed_candidate_edges)
            else:
                w_init = self.greedy_initialization(nb_candidates_to_choose,
                                                    rekeyed_candidate_edges)

            # Solver
            mac = MAC(rekeyed_fixed_edges, rekeyed_candidate_edges,
                      self.total_nb_poses)
            trial = 0
            while trial < nb_candidates_to_choose:
                try:
                    result, _, _ = mac.fw_subset(w_init,
                                                 nb_candidates_to_choose,
                                                 max_iters=self.max_iters)
                    break
                except:
                    # This should happend very rarely.
                    # find_fieldler_pair triggers a singular matrix exception
                    # when the mac select measurements leading to graph disconnection.
                    # Once at least one measurement is fixed connecting each robot it won't happen.
                    # We vary with increasing randomness the initial guess until we reach a viable solution.
                    trial = trial + 1
                    w_init = self.pseudo_greedy_initialization(
                        nb_candidates_to_choose, trial, rekeyed_candidate_edges)
                    continue

            if trial >= nb_candidates_to_choose:
                return []

            selected_edges = [
                rekeyed_candidate_edges[i]
                for i in np.nonzero(result.astype(int))[0]
            ]
            # Return selected multi-robot edges
            return self.recover_inter_robot_edges(selected_edges)
        else:
            return []

    def add_match(self, match):
        """Add match if the weight is 
            higher than the current best candidate associated 
            to a local keyframe

        Args:
            match (EdgeInterRobot): potential match
        """
        key = (match.robot0_image_id, match.robot1_id)
        if key in self.candidate_edges:
            if match.weight > self.candidate_edges[key].weight:
                self.add_candidate_edge(match)
        else:
            self.add_candidate_edge(match)
